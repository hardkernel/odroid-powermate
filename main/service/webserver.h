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
    size_t queue_depth;    // /uart: bytes waiting in the UART ring (= uart_buffered_bytes)
    size_t queue_capacity; // /uart: the ring's size
    size_t uart_buffered_bytes;
    uint32_t uart_received_bytes;     // read out of the ring = frame + shed + no-viewer bytes
    uint32_t uart_fifo_overflows;     // overflow episodes with a FIFO overrun (bytes lost uncounted)
    uint32_t uart_buffer_full_events; // overflow episodes with a full ring
    uint32_t uart_queue_drops;        // shed events + (viewer, frame) losses
    uint32_t uart_frame_bytes;        // read into frames for the viewers
    uint32_t uart_shed_bytes;         // discarded: the ring was nearly full and no frame went out
    uint32_t uart_no_viewer_bytes;    // discarded: no /uart viewer
    uint32_t uart_viewer_drop_bytes;  // per viewer: frame bytes it did not get
    uint32_t uart_lagging_marks;      // viewers that were not ready past the hold cap
    uint32_t uart_kick_failures;      // HTTPS: queuing the /uart drain failed (retried)
    uint32_t status_queue_drops; // = status_events_missed (there is no status queue)
    uint32_t websocket_send_failures;
    // Status frame drops by cause. Only status_drops_no_client has a drop point left: events
    // stored while no /ws session was open. The others are always 0.
    uint32_t status_drops_queue_full;
    uint32_t status_drops_https_pending;
    uint32_t status_drops_ctrl_queue;
    uint32_t status_drops_no_memory;
    uint32_t status_drops_no_client;
    uint32_t status_sw_drops;
    uint32_t status_https_pending; // always 0
    uint32_t uart_https_pending;   // HTTPS: /uart drain items queued and not started (0 over HTTP)
    // Switch and Wi-Fi state frames (never queued).
    uint32_t status_state_frames;        // written
    uint32_t status_state_owed;          // /ws sessions still owed state after the last pass
    uint32_t status_state_kick_failures; // HTTPS: queuing the state pass failed
    uint32_t status_state_kick_rearms;   // HTTPS: a queued state pass never ran
    uint32_t websocket_close_failures;   // closing a failed session could not be queued (retried)
    uint32_t websocket_close_retries;    // such closes retried
    uint32_t websocket_not_ready_skips;  // writes skipped (per client): the socket was not writable
    uint32_t websocket_heap_skips;       // HTTPS writes skipped (per client): no heap for the record
    uint32_t websocket_stall_closes;     // sessions closed after 20 s of not being ready
    // The /ws event and sensor lanes. "Per session" counts add up over every /ws session.
    uint32_t status_events_stored;           // events stored since boot
    uint32_t status_events_missed;           // per session: evicted before it got them (sent a marker)
    uint32_t status_events_lost_on_close;    // per session: not delivered when it ended (any reason)
    uint32_t status_sensor_merged;           // per session: samples skipped, more than 3 were pending
    uint32_t status_event_backlog_max;       // the most events one session had pending (evicted too)
    uint32_t status_event_backlog_max_bytes; // the most bytes of held events one session had pending
    uint32_t status_batches;                 // status writes (one send, one TLS record each)
    uint32_t status_stall_closes;            // /ws sessions closed after 20 s of not being ready
} websocket_diagnostics_t;

void register_wifi_endpoint(httpd_handle_t server);
void register_ws_endpoint(httpd_handle_t server);
void register_control_endpoint(httpd_handle_t server);
void register_diagnostics_endpoint(httpd_handle_t server);
/** Stores an encoded event or sensor_data StatusMessage for every open /ws session. Any task,
 *  never an ISR; never blocks or allocates. */
void push_data_to_ws(const uint8_t* data, size_t len);
/** The load switch outputs may have changed (or a request finished): every /ws client is sent
 *  the outputs as they are when its frame is written. Any task, never an ISR; never blocks. */
void ws_publish_switch_state(void);
/** Stores an encoded wifi_status StatusMessage as the latest Wi-Fi state (at most 80 B; a longer
 *  frame is refused with a log line and the previous one is kept). Any task; never blocks. */
void ws_publish_wifi_status(const uint8_t* frame, size_t len);
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
