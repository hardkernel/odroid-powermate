#include "auth.h"
#include "cJSON.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "webserver.h"

static esp_err_t diagnostics_get_handler(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
        return err;

    websocket_diagnostics_t ws_diagnostics;
    websocket_get_diagnostics(&ws_diagnostics);

    size_t client_count = POWERMATE_HTTP_MAX_OPEN_SOCKETS;
    int client_fds[POWERMATE_HTTP_MAX_OPEN_SOCKETS];
    size_t websocket_client_count = 0;
    if (httpd_get_client_list(req->handle, &client_count, client_fds) == ESP_OK)
    {
        for (size_t i = 0; i < client_count; ++i)
        {
            if (httpd_ws_get_fd_info(req->handle, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
                websocket_client_count++;
        }
    }
    else
    {
        client_count = 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root)
    {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block_bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "http_clients", client_count);
    cJSON_AddNumberToObject(root, "websocket_clients", websocket_client_count);
    cJSON_AddNumberToObject(root, "websocket_queue_depth", ws_diagnostics.queue_depth);
    cJSON_AddNumberToObject(root, "websocket_queue_capacity", ws_diagnostics.queue_capacity);
    cJSON_AddNumberToObject(root, "websocket_send_failures", ws_diagnostics.websocket_send_failures);
    cJSON_AddNumberToObject(root, "uart_buffered_bytes", ws_diagnostics.uart_buffered_bytes);
    cJSON_AddNumberToObject(root, "uart_received_bytes", ws_diagnostics.uart_received_bytes);
    cJSON_AddNumberToObject(root, "uart_fifo_overflows", ws_diagnostics.uart_fifo_overflows);
    cJSON_AddNumberToObject(root, "uart_buffer_full_events", ws_diagnostics.uart_buffer_full_events);
    cJSON_AddNumberToObject(root, "uart_queue_drops", ws_diagnostics.uart_queue_drops);
    cJSON_AddNumberToObject(root, "status_queue_drops", ws_diagnostics.status_queue_drops);

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        cJSON_AddBoolToObject(root, "wifi_connected", true);
        cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
    }
    else
    {
        cJSON_AddBoolToObject(root, "wifi_connected", false);
    }

    char* response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!response)
    {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

void register_diagnostics_endpoint(httpd_handle_t server)
{
    httpd_uri_t diagnostics = {
        .uri = "/api/diagnostics",
        .method = HTTP_GET,
        .handler = diagnostics_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &diagnostics);
}
