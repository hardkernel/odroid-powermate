//
// Created by shinys on 25. 8. 18..
//

#include "cJSON.h"
#include "webserver.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nconfig.h"
#include "driver/uart.h"
#include "freertos/semphr.h"

#define UART_NUM UART_NUM_1
#define BUF_SIZE (4096)
#define RD_BUF_SIZE (BUF_SIZE)
#define UART_TX_PIN CONFIG_GPIO_UART_TX
#define UART_RX_PIN CONFIG_GPIO_UART_RX

static const char *TAG = "ws-uart";

static int client_fd = -1;
static SemaphoreHandle_t client_fd_mutex;

struct status_message
{
    cJSON *data;
};

struct uart_to_ws_message
{
    uint8_t *data;
    size_t len;
};

QueueHandle_t status_queue;
static QueueHandle_t uart_to_ws_queue;

// Status task
static void status_task(void *arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;
    struct status_message msg;
    const TickType_t PING_INTERVAL = pdMS_TO_TICKS(5000);

    while (1) {
        if (xQueueReceive(status_queue, &msg, PING_INTERVAL)) {
            xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
            int fd = client_fd;
            xSemaphoreGive(client_fd_mutex);

            if (fd <= 0) {
                cJSON_Delete(msg.data);
                continue;
            }

            char *json_string = cJSON_Print(msg.data);
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = (uint8_t *)json_string;
            ws_pkt.len = strlen(json_string);
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
            esp_err_t err = httpd_ws_send_frame_async(server, fd, &ws_pkt);
            free(json_string);
            cJSON_Delete(msg.data);

            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "status_task: async send failed for fd %d, error: %s", fd, esp_err_to_name(err));
                xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
                if (client_fd == fd) {
                    client_fd = -1;
                }
                xSemaphoreGive(client_fd_mutex);
            }
        } else {
            // Queue receive timed out, send a PING to keep connection alive
            xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
            int fd = client_fd;
            xSemaphoreGive(client_fd_mutex);

            if (fd > 0) {
                httpd_ws_frame_t ping_pkt;
                memset(&ping_pkt, 0, sizeof(httpd_ws_frame_t));
                ping_pkt.type = HTTPD_WS_TYPE_PING;
                ping_pkt.final = true;
                if (httpd_ws_send_frame_async(server, fd, &ping_pkt) != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to send PING frame, closing connection for fd %d", fd);
                    xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
                    if (client_fd == fd) {
                        client_fd = -1;
                    }
                    xSemaphoreGive(client_fd_mutex);
                }
            }
        }
    }
}

static void ws_sender_task(void *arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;
    struct uart_to_ws_message msg;

    while (1) {
        if (xQueueReceive(uart_to_ws_queue, &msg, portMAX_DELAY)) {
            xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
            int fd = client_fd;
            xSemaphoreGive(client_fd_mutex);

            if (fd > 0) {
                httpd_ws_frame_t ws_pkt = {0};
                ws_pkt.payload = msg.data;
                ws_pkt.len = msg.len;
                ws_pkt.type = HTTPD_WS_TYPE_BINARY;

                esp_err_t err = httpd_ws_send_frame_async(server, fd, &ws_pkt);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "ws_sender_task: async send failed for fd %d, error: %s", fd, esp_err_to_name(err));
                    xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
                    if (client_fd == fd) {
                        client_fd = -1;
                    }
                    xSemaphoreGive(client_fd_mutex);
                }
            }
            free(msg.data);
        }
    }
}

static void uart_polling_task(void *arg)
{
    uint8_t* data_buf = (uint8_t*) malloc(RD_BUF_SIZE);
    if (data_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for UART polling buffer");
        vTaskDelete(NULL);
        return;
    }

    const TickType_t POLLING_INTERVAL = pdMS_TO_TICKS(10);

    while(1) {
        size_t available_len;
        uart_get_buffered_data_len(UART_NUM, &available_len);

        if (available_len > 0) {
            xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
            int current_fd = client_fd;
            xSemaphoreGive(client_fd_mutex);

            if (current_fd > 0) {
                // Read a chunk of data, up to the buffer size
                size_t read_size = available_len > RD_BUF_SIZE ? RD_BUF_SIZE : available_len;
                int bytes_read = uart_read_bytes(UART_NUM, data_buf, read_size, POLLING_INTERVAL);

                if (bytes_read > 0) {
                    struct uart_to_ws_message msg;
                    msg.data = malloc(bytes_read);
                    if (msg.data) {
                        memcpy(msg.data, data_buf, bytes_read);
                        msg.len = bytes_read;
                        // Use a small timeout to apply back-pressure if the queue is full
                        if (xQueueSend(uart_to_ws_queue, &msg, pdMS_TO_TICKS(10)) != pdPASS) {
                            ESP_LOGW(TAG, "ws sender queue full, dropping data");
                            free(msg.data);
                        }
                    } else {
                         ESP_LOGE(TAG, "Failed to allocate memory for uart ws msg");
                    }
                }
            } else {
                // No client connected, just discard the data
                uart_flush_input(UART_NUM);
            }
        }
        vTaskDelay(POLLING_INTERVAL);
    }
    free(data_buf);
    vTaskDelete(NULL);
}

// 웹소켓 처리 핸들러
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
        if (client_fd > 0) {
            // A client is already connected. Reject the new connection.
            ESP_LOGW(TAG, "Another client tried to connect, but a session is already active. Rejecting.");
            xSemaphoreGive(client_fd_mutex);
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Another client is already connected");
            return ESP_FAIL;
        }

        // No client is connected. Accept the new one.
        int new_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Accepting new websocket connection: %d", new_fd);
        client_fd = new_fd;
        xSemaphoreGive(client_fd_mutex);

        // Reset queues and flush UART buffer for the new session
        xQueueReset(status_queue);
        xQueueReset(uart_to_ws_queue);
        uart_flush_input(UART_NUM);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    uint8_t buf[BUF_SIZE];
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, BUF_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "httpd_ws_recv_frame failed with error: %s", esp_err_to_name(ret));
        xSemaphoreTake(client_fd_mutex, portMAX_DELAY);
        if (httpd_req_to_sockfd(req) == client_fd) {
            client_fd = -1;
        }
        xSemaphoreGive(client_fd_mutex);
        return ret;
    }

    uart_write_bytes(UART_NUM, (const char *)ws_pkt.payload, ws_pkt.len);

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
        // .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));

    httpd_uri_t ws = {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = ws_handler,
        .user_ctx  = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws);

    client_fd_mutex = xSemaphoreCreateMutex();
    status_queue = xQueueCreate(10, sizeof(struct status_message));
    uart_to_ws_queue = xQueueCreate(50, sizeof(struct uart_to_ws_message));

    xTaskCreate(uart_polling_task, "uart_polling_task", 1024*4, NULL, 8, NULL);
    xTaskCreate(status_task, "status_task", 4096, server, 8, NULL);
    xTaskCreate(ws_sender_task, "ws_sender_task", 1024*6, server, 9, NULL);
}

void push_data_to_ws(cJSON *data)
{
    struct status_message msg;
    msg.data = data;
    if (xQueueSend(status_queue, &msg, 10) != pdPASS)
    {
        ESP_LOGW(TAG, "Queue full");
    }
}

esp_err_t change_baud_rate(int baud_rate)
{
    return uart_set_baudrate(UART_NUM, baud_rate);
}