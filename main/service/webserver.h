//
// Created by shinys on 25. 8. 18..
//

#ifndef ODROID_REMOTE_HTTP_WEBSERVER_H
#define ODROID_REMOTE_HTTP_WEBSERVER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#define POWERMATE_HTTP_MAX_OPEN_SOCKETS 7

typedef struct
{
    size_t queue_depth;
    size_t queue_capacity;
    size_t uart_buffered_bytes;
    uint32_t uart_received_bytes;
    uint32_t uart_fifo_overflows;
    uint32_t uart_buffer_full_events;
    uint32_t uart_queue_drops;
    uint32_t status_queue_drops;
    uint32_t websocket_send_failures;
} websocket_diagnostics_t;

void register_wifi_endpoint(httpd_handle_t server);
void register_ws_endpoint(httpd_handle_t server);
void register_control_endpoint(httpd_handle_t server);
void register_diagnostics_endpoint(httpd_handle_t server);
void push_data_to_ws(const uint8_t* data, size_t len);
void websocket_get_diagnostics(websocket_diagnostics_t* diagnostics);
void register_reboot_endpoint(httpd_handle_t server);
esp_err_t change_baud_rate(int baud_rate);
void register_version_endpoint(httpd_handle_t server);

#endif // ODROID_REMOTE_HTTP_WEBSERVER_H
