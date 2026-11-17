/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 25. 8. 18..
//

#ifndef ODROID_REMOTE_HTTP_WEBSERVER_H
#define ODROID_REMOTE_HTTP_WEBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

// Must stay <= CONFIG_LWIP_MAX_SOCKETS - 3 (httpd's own sockets). 10 holds three
// browsers (two WebSockets each) plus the requests of a page load.
#define POWERMATE_HTTP_MAX_OPEN_SOCKETS 10
// HTTPS sessions. With mbedTLS dynamic buffers an idle TLS session holds ~4 KB; six
// fit a browser and a client (two WebSockets each) plus requests, with ~84 KB free.
#define POWERMATE_HTTPS_MAX_OPEN_SOCKETS 6

// Web UI login. JSON escaping can grow a value up to six times, so the login body limit
// leaves room for both at full length.
#define WEB_USERNAME_MAX_LEN 32
#define WEB_PASSWORD_MAX_LEN 64
#define WEB_LOGIN_MAX_BODY 1024

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
bool uart_baud_rate_is_supported(long baud_rate);
esp_err_t change_baud_rate(int baud_rate);
void register_version_endpoint(httpd_handle_t server);
esp_err_t register_tls_endpoint(httpd_handle_t server);

/**
 * Reads the whole request body (Content-Length bytes, which may arrive in several pieces)
 * into a NUL-terminated heap buffer. Refuses an empty body or one over max_len, and sends
 * the error response itself; returns NULL then. Free the buffer with webserver_free_body().
 */
char* webserver_read_body(httpd_req_t* req, size_t max_len);
/** Wipes (it may hold a password) and frees a buffer from webserver_read_body(). */
void webserver_free_body(char* body);
/** True when the web server runs over HTTPS (the setting is on and the certificate loaded). */
bool webserver_is_https(void);

#endif // ODROID_REMOTE_HTTP_WEBSERVER_H
