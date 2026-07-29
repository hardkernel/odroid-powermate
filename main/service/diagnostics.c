#include "auth.h"
#include "cJSON.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nconfig.h"
#include "webserver.h"
#include "wifi.h"

static const char* wifi_sta_state_str(wifi_sta_connection_state_t state)
{
    switch (state)
    {
    case WIFI_STA_CONNECTION_CONNECTING:
        return "connecting";
    case WIFI_STA_CONNECTION_CONNECTED:
        return "connected";
    case WIFI_STA_CONNECTION_FAILED:
        return "failed";
    case WIFI_STA_CONNECTION_IDLE:
    default:
        return "idle";
    }
}

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

    wifi_sta_diagnostics_t wifi_diagnostics;
    wifi_get_sta_diagnostics(&wifi_diagnostics);
    cJSON_AddStringToObject(root, "wifi_sta_state",
                            wifi_sta_state_str(wifi_diagnostics.connection_state));
    cJSON_AddStringToObject(
        root, "wifi_last_disconnect_reason",
        wifi_diagnostics.last_disconnect_reason == WIFI_REASON_UNSPECIFIED
            ? ""
            : wifi_reason_str(wifi_diagnostics.last_disconnect_reason));
    cJSON_AddNumberToObject(root, "wifi_last_disconnect_reason_code",
                            wifi_diagnostics.last_disconnect_reason);
    cJSON_AddNumberToObject(
        root, "wifi_reconnect_backoff_ms",
        wifi_diagnostics.connection_state == WIFI_STA_CONNECTION_CONNECTING
            ? wifi_diagnostics.reconnect_backoff_ms
            : 0);
    cJSON_AddBoolToObject(root, "wifi_has_connected", wifi_diagnostics.has_connected);
    cJSON_AddNumberToObject(root, "wifi_last_connected_uptime_seconds",
                            wifi_diagnostics.last_connected_uptime_seconds);

    char net_type[16] = "dhcp";
    nconfig_read(NETIF_TYPE, net_type, sizeof(net_type));
    cJSON_AddStringToObject(root, "wifi_net_type", net_type);

    wifi_ap_record_t ap_info;
    bool wifi_connected = wifi_get_current_ap_info(&ap_info) == ESP_OK;
    if (wifi_connected)
    {
        cJSON_AddBoolToObject(root, "wifi_connected", true);
        cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
    }
    else
    {
        cJSON_AddBoolToObject(root, "wifi_connected", false);
    }

    char ip_address[16] = "";
    char gateway[16] = "";
    char netmask[16] = "";
    esp_netif_ip_info_t ip_info;
    if (wifi_diagnostics.connection_state == WIFI_STA_CONNECTION_CONNECTED &&
        wifi_get_current_ip_info(&ip_info) == ESP_OK)
    {
        esp_ip4addr_ntoa(&ip_info.ip, ip_address, sizeof(ip_address));
        esp_ip4addr_ntoa(&ip_info.gw, gateway, sizeof(gateway));
        esp_ip4addr_ntoa(&ip_info.netmask, netmask, sizeof(netmask));
    }
    cJSON_AddStringToObject(root, "wifi_ip_address", ip_address);
    cJSON_AddStringToObject(root, "wifi_gateway", gateway);
    cJSON_AddStringToObject(root, "wifi_netmask", netmask);

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
