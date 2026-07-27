//
// Created by shinys on 25. 8. 18..
//

#include "auth.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nconfig.h"
#include <stdlib.h>
#include "string.h"
#include "webserver.h"

#define UART_NUM UART_NUM_1
#define BUF_SIZE 2048
#define UART_RX_BUFFER_SIZE (16 * 1024)
#define UART_TX_BUFFER_SIZE 2048
#define UART_WS_QUEUE_LENGTH 8
#define UART_MIN_FREE_HEAP (48 * 1024)
#define UART_TX_PIN CONFIG_GPIO_UART_TX
#define UART_RX_PIN CONFIG_GPIO_UART_RX

static const char* TAG = "ws-uart";

struct ws_message
{
    uint8_t* data;
    size_t len;
};

struct ws_sender_config
{
    QueueHandle_t queue;
    httpd_handle_t server;
    bool uart_stream;
    const char* name;
};

#define MAX_CLIENT POWERMATE_HTTP_MAX_OPEN_SOCKETS
static QueueHandle_t status_ws_queue;
static QueueHandle_t uart_ws_queue;
static QueueHandle_t uart_event_queue;
static struct ws_sender_config status_sender;
static struct ws_sender_config uart_sender;
static volatile uint32_t uart_received_bytes;
static volatile uint32_t uart_fifo_overflows;
static volatile uint32_t uart_buffer_full_events;
static volatile uint32_t uart_queue_drops;
static volatile uint32_t status_queue_drops;
static volatile uint32_t websocket_send_failures;
static int blocked_ws_fds[MAX_CLIENT];
static uint8_t status_ws_session;
static uint8_t uart_ws_session;

static void websocket_session_ctx_free(void* ctx)
{
}

static bool fd_in_list(const int* fds, int fd)
{
    for (size_t i = 0; i < MAX_CLIENT; ++i)
    {
        if (fds[i] == fd)
            return true;
    }
    return false;
}

static void add_fd_to_list(int* fds, int fd)
{
    for (size_t i = 0; i < MAX_CLIENT; ++i)
    {
        if (fds[i] == fd)
            return;
        if (fds[i] == 0)
        {
            fds[i] = fd;
            return;
        }
    }
}

static void remove_fd_from_list(int* fds, int fd)
{
    for (size_t i = 0; i < MAX_CLIENT; ++i)
    {
        if (fds[i] == fd)
            fds[i] = 0;
    }
}

static void cleanup_fd_list(int* fds, const int* client_fds, size_t clients)
{
    for (size_t i = 0; i < MAX_CLIENT; ++i)
    {
        if (fds[i] == 0)
            continue;

        bool still_connected = false;
        for (size_t j = 0; j < clients; ++j)
        {
            if (fds[i] == client_fds[j])
            {
                still_connected = true;
                break;
            }
        }
        if (!still_connected)
            fds[i] = 0;
    }
}

static void cleanup_client_fds(const int* client_fds, size_t clients)
{
    cleanup_fd_list(blocked_ws_fds, client_fds, clients);
}

static bool uart_websocket_client_connected(httpd_handle_t server)
{
    int client_fds[MAX_CLIENT];
    size_t clients = MAX_CLIENT;

    if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK)
        return false;

    cleanup_client_fds(client_fds, clients);
    for (size_t i = 0; i < clients; ++i)
    {
        if (httpd_sess_get_ctx(server, client_fds[i]) == &uart_ws_session && !fd_in_list(blocked_ws_fds, client_fds[i]) &&
            httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            return true;
    }

    return false;
}

static bool status_websocket_client_connected(httpd_handle_t server)
{
    int client_fds[MAX_CLIENT];
    size_t clients = MAX_CLIENT;

    if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK)
        return false;

    cleanup_client_fds(client_fds, clients);
    for (size_t i = 0; i < clients; ++i)
    {
        if (httpd_sess_get_ctx(server, client_fds[i]) == &status_ws_session && !fd_in_list(blocked_ws_fds, client_fds[i]) &&
            httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            return true;
    }

    return false;
}

static void ws_sender_task(void* arg)
{
    const struct ws_sender_config* config = arg;
    httpd_handle_t server = config->server;
    struct ws_message msg;
    int client_fds[MAX_CLIENT];

    while (1)
    {
        if (xQueueReceive(config->queue, &msg, portMAX_DELAY) != pdPASS)
            continue;

        if (config->uart_stream && heap_caps_get_free_size(MALLOC_CAP_8BIT) < UART_MIN_FREE_HEAP)
        {
            uart_queue_drops++;
            free(msg.data);
            continue;
        }

        size_t clients = MAX_CLIENT;
        if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK)
        {
            cleanup_client_fds(client_fds, clients);

            httpd_ws_frame_t frame = {
                .payload = msg.data,
                .len = msg.len,
                .type = HTTPD_WS_TYPE_BINARY,
            };

            for (size_t i = 0; i < clients; ++i)
            {
                int fd = client_fds[i];
                if (httpd_sess_get_ctx(server, fd) != (config->uart_stream ? (void*)&uart_ws_session : (void*)&status_ws_session) ||
                    fd_in_list(blocked_ws_fds, fd) ||
                    httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
                    continue;

                esp_err_t err = httpd_ws_send_frame_async(server, fd, &frame);
                if (err != ESP_OK)
                {
                    websocket_send_failures++;
                    add_fd_to_list(blocked_ws_fds, fd);
                    ESP_LOGW(TAG, "%s: send failed for fd %d: %s", config->name, fd, esp_err_to_name(err));
                    httpd_sess_trigger_close(server, fd);
                }
            }
        }
        free(msg.data);
    }
}

static void uart_polling_task(void* arg)
{
    httpd_handle_t server = arg;
    static uint8_t data_buf[BUF_SIZE];

    while (1)
    {
        size_t available_len;
        uart_get_buffered_data_len(UART_NUM, &available_len);
        if (available_len == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t read_len = available_len < sizeof(data_buf) ? available_len : sizeof(data_buf);
        int bytes_read = uart_read_bytes(UART_NUM, data_buf, read_len, 0);
        if (bytes_read <= 0)
            continue;

        uart_received_bytes += bytes_read;

        if (!uart_websocket_client_connected(server))
            continue;

        if (uxQueueSpacesAvailable(uart_ws_queue) == 0 ||
            heap_caps_get_free_size(MALLOC_CAP_8BIT) < UART_MIN_FREE_HEAP)
        {
            uart_queue_drops++;
            continue;
        }

        struct ws_message msg = {
            .data = malloc(bytes_read),
            .len = bytes_read,
        };
        if (!msg.data)
        {
            uart_queue_drops++;
            continue;
        }
        memcpy(msg.data, data_buf, bytes_read);

        if (xQueueSend(uart_ws_queue, &msg, 0) != pdPASS)
        {
            uart_queue_drops++;
            free(msg.data);
        }
    }
}

static void uart_event_task(void* arg)
{
    uart_event_t event;
    while (1)
    {
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY) != pdPASS)
            continue;

        if (event.type == UART_FIFO_OVF)
        {
            uart_fifo_overflows++;
            ESP_LOGW(TAG, "UART HW FIFO Overflow");
            uart_flush_input(UART_NUM);
            xQueueReset(uart_event_queue);
        }
        else if (event.type == UART_BUFFER_FULL)
        {
            uart_buffer_full_events++;
            ESP_LOGW(TAG, "UART ring buffer full");
            uart_flush_input(UART_NUM);
            xQueueReset(uart_event_queue);
        }
    }
}

static esp_err_t websocket_handshake(httpd_req_t* req, bool uart_stream)
{
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len <= 1)
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Authorization token required");
        return ESP_FAIL;
    }

    char* query = malloc(query_len);
    char token[TOKEN_LENGTH];
    if (!query || httpd_req_get_url_query_str(req, query, query_len) != ESP_OK ||
        httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK || !auth_validate_token(token))
    {
        free(query);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid or expired token");
        return ESP_FAIL;
    }
    free(query);

    int fd = httpd_req_to_sockfd(req);
    remove_fd_from_list(blocked_ws_fds, fd);
    req->sess_ctx = uart_stream ? (void*)&uart_ws_session : (void*)&status_ws_session;
    req->free_ctx = websocket_session_ctx_free;

    return ESP_OK;
}

static esp_err_t ws_pre_handshake_cb(httpd_req_t* req)
{
    return websocket_handshake(req, req->user_ctx == &uart_ws_session);
}

static esp_err_t ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET)
        return websocket_handshake(req, false);

    httpd_ws_frame_t frame = {0};
    uint8_t buffer[BUF_SIZE];
    frame.payload = buffer;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buffer));
    if (err != ESP_OK)
        return err;

    if (frame.type == HTTPD_WS_TYPE_TEXT && frame.len == strlen("ping") &&
        strncmp((const char*)frame.payload, "ping", frame.len) == 0)
    {
        httpd_ws_frame_t pong = {.payload = (uint8_t*)"pong", .len = strlen("pong"), .type = HTTPD_WS_TYPE_TEXT};
        return httpd_ws_send_frame(req, &pong);
    }
    return ESP_OK;
}

static esp_err_t uart_ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET)
        return websocket_handshake(req, true);

    httpd_ws_frame_t frame = {0};
    uint8_t buffer[BUF_SIZE];
    frame.payload = buffer;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buffer));
    if (err != ESP_OK)
        return err;

    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_TEXT || frame.type == HTTPD_WS_TYPE_BINARY)
        uart_write_bytes(UART_NUM, (const char*)frame.payload, frame.len);

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
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_RX_BUFFER_SIZE, UART_TX_BUFFER_SIZE, 20, &uart_event_queue, 0));

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = &status_ws_session,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake_cb,
    };
    httpd_uri_t uart_ws = {
        .uri = "/uart",
        .method = HTTP_GET,
        .handler = uart_ws_handler,
        .user_ctx = &uart_ws_session,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake_cb,
    };
    httpd_register_uri_handler(server, &ws);
    httpd_register_uri_handler(server, &uart_ws);

    status_ws_queue = xQueueCreate(10, sizeof(struct ws_message));
    uart_ws_queue = xQueueCreate(UART_WS_QUEUE_LENGTH, sizeof(struct ws_message));
    status_sender = (struct ws_sender_config){
        .queue = status_ws_queue, .server = server, .uart_stream = false, .name = "ws-status"};
    uart_sender = (struct ws_sender_config){
        .queue = uart_ws_queue, .server = server, .uart_stream = true, .name = "ws-uart"};
    xTaskCreate(uart_polling_task, "uart_polling_task", 1024 * 4, server, 8, NULL);
    xTaskCreate(ws_sender_task, "ws_status_sender", 1024 * 6, &status_sender, 9, NULL);
    xTaskCreate(ws_sender_task, "ws_uart_sender", 1024 * 6, &uart_sender, 9, NULL);
    xTaskCreate(uart_event_task, "uart_event_task", 1024 * 2, NULL, 10, NULL);
}

void push_data_to_ws(const uint8_t* data, size_t len)
{
    if (!status_websocket_client_connected(status_sender.server))
        return;

    struct ws_message msg = {.data = malloc(len), .len = len};
    if (!msg.data)
        return;
    memcpy(msg.data, data, len);

    if (xQueueSend(status_ws_queue, &msg, 0) != pdPASS)
    {
        status_queue_drops++;
        free(msg.data);
    }
}

void websocket_get_diagnostics(websocket_diagnostics_t* diagnostics)
{
    if (!diagnostics)
        return;

    memset(diagnostics, 0, sizeof(*diagnostics));
    if (uart_ws_queue)
    {
        diagnostics->queue_depth = uxQueueMessagesWaiting(uart_ws_queue);
        diagnostics->queue_capacity = diagnostics->queue_depth + uxQueueSpacesAvailable(uart_ws_queue);
    }
    if (uart_event_queue)
        uart_get_buffered_data_len(UART_NUM, &diagnostics->uart_buffered_bytes);

    diagnostics->uart_received_bytes = uart_received_bytes;
    diagnostics->uart_fifo_overflows = uart_fifo_overflows;
    diagnostics->uart_buffer_full_events = uart_buffer_full_events;
    diagnostics->uart_queue_drops = uart_queue_drops;
    diagnostics->status_queue_drops = status_queue_drops;
    diagnostics->websocket_send_failures = websocket_send_failures;
}

esp_err_t change_baud_rate(int baud_rate)
{
    return uart_set_baudrate(UART_NUM, baud_rate);
}
