//
// Created by shinys on 25. 8. 18..
//

#include "auth.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nconfig.h"
#include "pb.h"
#include "pb_encode.h"
#include "status.pb.h"
#include "webserver.h"

#define UART_NUM UART_NUM_1
#define BUF_SIZE (2048)
#define UART_TX_PIN CONFIG_GPIO_UART_TX
#define UART_RX_PIN CONFIG_GPIO_UART_RX
#define CHUNK_SIZE (2048)
#define PB_UART_BUFFER_SIZE (CHUNK_SIZE + 64)

static const char* TAG = "ws-uart";

enum ws_message_type
{
    WS_MSG_STATUS,
    WS_MSG_UART
};

struct ws_message
{
    enum ws_message_type type;
    uint8_t* data;
    size_t len;
};

struct bytes_arg
{
    const void* data;
    size_t len;
};

#define MAX_CLIENT 7
static QueueHandle_t ws_queue;
static QueueHandle_t uart_event_queue;
static int client_fds[MAX_CLIENT];

static bool encode_bytes_callback(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    struct bytes_arg* br = (struct bytes_arg*)(*arg);
    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }
    return pb_encode_string(stream, (uint8_t*)br->data, br->len);
}

static void unified_ws_sender_task(void* arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;
    struct ws_message msg;

    while (1)
    {
        if (xQueueReceive(ws_queue, &msg, portMAX_DELAY))
        {
            size_t clients = MAX_CLIENT;
            if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK)
            {
                free(msg.data);
                continue;
            }

            if (clients == 0)
            {
                free(msg.data);
                continue;
            }

            httpd_ws_frame_t ws_pkt = {0};
            ws_pkt.payload = msg.data;
            ws_pkt.len = msg.len;
            ws_pkt.type = HTTPD_WS_TYPE_BINARY;

            for (size_t i = 0; i < clients; ++i)
            {
                int fd = client_fds[i];
                if (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET)
                {
                    esp_err_t err = httpd_ws_send_frame_async(server, fd, &ws_pkt);
                    if (err != ESP_OK)
                    {
                        ESP_LOGW(TAG, "unified_ws_sender_task: async send failed for fd %d, error: %s", fd,
                                 esp_err_to_name(err));
                    }
                }
            }
            free(msg.data);
        }
    }
    free(client_fds);
    vTaskDelete(NULL);
}

static void uart_polling_task(void* arg)
{
    static uint8_t data_buf[BUF_SIZE];
    static uint8_t pb_buffer[PB_UART_BUFFER_SIZE];

    while (1)
    {
        size_t available_len;
        uart_get_buffered_data_len(UART_NUM, &available_len);

        if (available_len == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t read_len = (available_len > BUF_SIZE) ? BUF_SIZE : available_len;
        int bytes_read = uart_read_bytes(UART_NUM, data_buf, read_len, pdMS_TO_TICKS(5));

        if (bytes_read > 0)
        {
            size_t offset = 0;
            while (offset < bytes_read)
            {
                size_t chunk_size = (bytes_read - offset > CHUNK_SIZE) ? CHUNK_SIZE : (bytes_read - offset);

                StatusMessage message = StatusMessage_init_zero;
                message.which_payload = StatusMessage_uart_data_tag;
                struct bytes_arg a = {.data = data_buf + offset, .len = chunk_size};
                message.payload.uart_data.data.funcs.encode = &encode_bytes_callback;
                message.payload.uart_data.data.arg = &a;

                pb_ostream_t stream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
                if (!pb_encode(&stream, StatusMessage_fields, &message))
                {
                    ESP_LOGE(TAG, "Failed to encode uart data: %s", PB_GET_ERROR(&stream));
                    offset += chunk_size;
                    continue;
                }

                struct ws_message msg;
                msg.type = WS_MSG_UART;
                msg.len = stream.bytes_written;
                msg.data = malloc(msg.len);

                if (!msg.data)
                {
                    ESP_LOGE(TAG, "Failed to allocate memory for uart ws msg");
                    offset += chunk_size;
                    continue;
                }

                memcpy(msg.data, pb_buffer, msg.len);

                if (xQueueSend(ws_queue, &msg, pdMS_TO_TICKS(10)) != pdPASS)
                {
                    ESP_LOGW(TAG, "ws sender queue full, dropping %zu bytes", chunk_size);
                    free(msg.data);
                }

                offset += chunk_size;
            }
        }
    }
    vTaskDelete(NULL);
}

static void uart_event_task(void* arg)
{
    uart_event_t event;
    while (1)
    {
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY))
        {
            switch (event.type)
            {
            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART HW FIFO Overflow");
                uart_flush_input(UART_NUM);
                xQueueReset(uart_event_queue);
                break;
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART ring buffer full");
                uart_flush_input(UART_NUM);
                xQueueReset(uart_event_queue);
                break;
            case UART_DATA:
                // Muting this event because it is too noisy
                break;
            default:
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "WebSocket GET request received for URI: %s", req->uri);

        char* query_str = NULL;
        size_t query_len = httpd_req_get_url_query_len(req) + 1;
        if (query_len > 1)
        {
            query_str = malloc(query_len);
            if (query_str == NULL)
            {
                ESP_LOGE(TAG, "Failed to allocate memory for query string");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error");
                return ESP_FAIL;
            }
            if (httpd_req_get_url_query_str(req, query_str, query_len) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to get query string from URI: %s", req->uri);
                free(query_str);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Extracted query string: %s", query_str);
        }

        char token_str[TOKEN_LENGTH];
        esp_err_t err = ESP_FAIL; // Default to fail

        if (query_str)
        {
            err = httpd_query_key_value(query_str, "token", token_str, sizeof(token_str));
            free(query_str); // Free allocated query string
        }

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Token extracted from query string, value: %s", token_str);
            if (!auth_validate_token(token_str))
            {
                ESP_LOGW(TAG, "WebSocket connection attempt with invalid token for URI: %s", req->uri);
                httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid or expired token");
                return ESP_FAIL;
            }
            ESP_LOGD(TAG, "WebSocket token validated for URI: %s", req->uri);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to extract token from query string or query string not found, error: %s",
                     esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Authorization token required");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    uint8_t buf[BUF_SIZE];
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, BUF_SIZE);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "httpd_ws_recv_frame failed with error: %s", esp_err_to_name(ret));
        return ret;
    }

    uart_write_bytes(UART_NUM, (const char*)ws_pkt.payload, ws_pkt.len);

    return ESP_OK;
}

void register_ws_endpoint(httpd_handle_t server)
{
    size_t baud_rate_len;
    nconfig_get_str_len(UART_BAUD_RATE, &baud_rate_len);
    char buf[baud_rate_len];
    nconfig_read(UART_BAUD_RATE, buf, baud_rate_len);

    uart_config_t uart_config = {
        .baud_rate = strtol(buf, NULL, 10),
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_event_queue, 0));

    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL, .is_websocket = true};
    httpd_register_uri_handler(server, &ws);

    ws_queue = xQueueCreate(10, sizeof(struct ws_message));

    xTaskCreate(uart_polling_task, "uart_polling_task", 1024 * 4, NULL, 8, NULL);
    xTaskCreate(unified_ws_sender_task, "ws_sender_task", 1024 * 6, server, 9, NULL);
    xTaskCreate(uart_event_task, "uart_event_task", 1024 * 2, NULL, 10, NULL);
}

void push_data_to_ws(const uint8_t* data, size_t len)
{
    struct ws_message msg;
    msg.type = WS_MSG_STATUS;
    msg.data = malloc(len);
    if (!msg.data)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for status ws msg");
        return;
    }
    memcpy(msg.data, data, len);
    msg.len = len;

    if (xQueueSend(ws_queue, &msg, pdMS_TO_TICKS(10)) != pdPASS)
    {
        ESP_LOGW(TAG, "WS queue full, dropping status message");
        free(msg.data);
    }
}

esp_err_t change_baud_rate(int baud_rate) { return uart_set_baudrate(UART_NUM, baud_rate); }
