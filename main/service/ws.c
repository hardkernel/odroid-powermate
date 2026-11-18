/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 25. 8. 18..
//

#include "auth.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "event.h" // encode_event: the missed-events marker
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/opt.h"     // TCP_SNDLOWAT, TCP_SNDQUEUELOWAT, TCP_SND_QUEUELEN
#include "lwip/sockets.h" // lwip_select (select() would go through esp_vfs_select, which callocs)
#include "nconfig.h"
#include "pbmsg.h" // PB_BUFFER_SIZE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "string.h"
#include "sw.h"
#include "webserver.h"
#include "ws_state.h"

#define UART_NUM UART_NUM_1
#define BUF_SIZE 2048
#define UART_RX_BUFFER_SIZE (16 * 1024)
#define UART_TX_BUFFER_SIZE 2048
#define UART_WS_QUEUE_LENGTH 8
#define UART_MIN_FREE_HEAP (48 * 1024)
#define WS_LRU_UPDATE_INTERVAL_MS 1000
#define WS_NOT_READY_REPOLL_MS 200 // a state pass has work left: run another after this
#define WS_HEAP_CLOSE_GAP_MS 2000  // at most one close per this of sessions stalled for heap only
#define WS_LRU_STALL_MS 5000       // a WebSocket unwritable and stalled this long loses its LRU refresh
#define UART_TX_PIN CONFIG_GPIO_UART_TX
#define UART_RX_PIN CONFIG_GPIO_UART_RX
#define UART_DEFAULT_BAUD_RATE 1500000

// The /ws snapshot is requested from the post-handshake callback. sdkconfig.defaults does not
// override a value set in an existing sdkconfig, and without the option httpd_uri_t has no
// ws_post_handshake_cb.
#if !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "sdkconfig predates CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT in sdkconfig.defaults: delete sdkconfig and rebuild"
#endif

// ---- Never block on a WebSocket write ---------------------------------------------------
// Every write ws.c makes is checked first: the socket must be writable (lwIP then takes at
// least TCP_SNDLOWAT bytes without waiting), and over HTTPS the heap must hold the TLS record.
// A write that would block is skipped. On /ws the skip itself loses nothing: owed state stays
// owed and the session's lane cursors stay put, so its frames go out with a later write. Only
// what the lanes drop meanwhile is lost: events the ring evicts (a marker reports how many) and
// samples older than the newest WS_SENSOR_SLOTS. On /uart the chunk is lost for that client.
// The one exception is /uart over HTTP. Its writer, the UART sender task, writes nothing else,
// so a blocking write there stalls only the console, as in v1.x. A check would keep at most
// TCP_SNDLOWAT + one chunk unacknowledged instead of the whole send buffer, and drop console
// output whenever ACKs take longer than ~40 ms at 1.5 Mbaud. A dead client fails its write
// after httpd's send timeout (5 s), and the session is closed.

#define WS_FRAME_HEADER_MAX 10         // httpd_ws.c: 2 B + 8 B extended length (server frames are not masked)
#define WS_TLS_RECORD_OVERHEAD_MAX 128 // TLS 1.2 header + explicit IV + MAC + padding, upper bound
// One readiness check covers at most sw_status + wifi_status + one status frame, each written as
// header + payload (2 writes, 2 TLS records), or one UART chunk.
#define WS_STATUS_WRITE_MAX                                                                         \
    (3 * WS_FRAME_HEADER_MAX + 16 + WS_WIFI_FRAME_MAX + PB_BUFFER_SIZE + 6 * WS_TLS_RECORD_OVERHEAD_MAX)
#define WS_UART_WRITE_MAX (WS_FRAME_HEADER_MAX + BUF_SIZE + 2 * WS_TLS_RECORD_OVERHEAD_MAX)
_Static_assert(WS_STATUS_WRITE_MAX < TCP_SNDLOWAT, "a status write after a readiness check could block");
_Static_assert(WS_UART_WRITE_MAX < TCP_SNDLOWAT, "a UART write after a readiness check could block");
// 6 writes (3 frames x 2) of at most one segment each, or 1 + ceil(2048 / MSS) for UART.
_Static_assert(TCP_SNDQUEUELOWAT - 1 + 6 <= TCP_SND_QUEUELEN, "the segment queue could fill after a check");
_Static_assert(BUF_SIZE <= 2 * TCP_MSS, "a UART chunk is assumed to need at most 2 segments");

// Every TLS record write allocates a TX buffer of 8 B + MBEDTLS_SSL_OUT_BUFFER_LEN (13 header +
// 16 IV + 48 MAC + 256 padding + 4096 content). The records of one call pin up to two MSS pbufs
// until ACKed (Nagle: the first record goes out, the rest wait in a second segment), a 2 KB
// UART chunk three.
#define WS_TLS_TX_BUF_LEN 4437
#define WS_PBUF_RESERVE 3072             // two MSS pbufs per status write
#define WS_UART_PBUF_RESERVE 4608        // three MSS pbufs per /uart fd
#define WS_TLS_WRITE_MIN_BLOCK 7680      // probe size per status write
#define WS_TLS_UART_MIN_BLOCK 9216       // probe size per /uart fd
#define WS_HEAP_PROBE_GATE (32 * 1024)   // no probe at or above this much free heap
_Static_assert(WS_TLS_WRITE_MIN_BLOCK >= WS_TLS_TX_BUF_LEN + WS_PBUF_RESERVE, "probe smaller than one fd's write");
_Static_assert(WS_TLS_UART_MIN_BLOCK >= WS_TLS_TX_BUF_LEN + WS_UART_PBUF_RESERVE, "probe smaller than one /uart write");
_Static_assert(WS_STATUS_WRITE_MAX <= TCP_MSS, "status records are assumed to fit the second segment (2 pbufs)");
_Static_assert(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN == 4096, "WS_TLS_TX_BUF_LEN assumes 4096 B TLS out content: recompute it");

static const char* TAG = "ws-uart";

// Keep in sync with the baud rate lists in page/index.html and example/pmtui/uart_menu.go.
static const long supported_baud_rates[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000,
};

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
    atomic_int pending; // HTTPS: frames handed to the httpd task and not sent yet
};

#define MAX_CLIENT POWERMATE_HTTP_MAX_OPEN_SOCKETS
// Every httpd session is an lwIP socket, and the per-fd state table (ws_state.c) has one slot
// per lwIP socket, so it covers every session.
_Static_assert(POWERMATE_HTTP_MAX_OPEN_SOCKETS + 3 <= CONFIG_LWIP_MAX_SOCKETS,
               "CONFIG_LWIP_MAX_SOCKETS too small for POWERMATE_HTTP_MAX_OPEN_SOCKETS: delete sdkconfig and rebuild");
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
static volatile uint32_t websocket_close_failures; // httpd_sess_trigger_close failed (retried)
static volatile uint32_t websocket_close_retries;  // closes re-triggered by a state pass
static volatile uint32_t websocket_not_ready_skips; // writes skipped: the socket was not writable
static volatile uint32_t websocket_heap_skips;      // HTTPS writes skipped: no heap for the TLS record
static volatile uint32_t websocket_stall_closes;    // sessions closed after WS_STALL_CLOSE_MS not ready
static bool ws_https; // webserver_is_https(), set by register_ws_endpoint
// Status frame drops by cause. Their drop points are gone: events and sensor samples are held in
// ws_state.c's lanes until sent, so these stay 0.
static volatile uint32_t status_drops_queue_full;
static volatile uint32_t status_drops_https_pending;
static volatile uint32_t status_drops_no_memory;
static volatile uint32_t status_drops_ctrl_queue;
static volatile uint32_t status_sw_drops;
// Switch and Wi-Fi state frames (see ws_state.h).
static volatile uint32_t state_frames;        // state frames written
static volatile uint32_t state_owed;          // gauge: /ws sessions still owed state after the last pass
static volatile uint32_t state_kick_failures; // HTTPS: queuing the state pass failed (retried)
static volatile uint32_t state_kick_rearms;   // HTTPS: a queued state pass never ran, kicked again
static TaskHandle_t volatile status_sender_task; // NULL until register_ws_endpoint created it
static uint8_t status_ws_session; // the /ws URI's user_ctx; each /ws session's ctx is ws_state_ctx(fd)
static uint8_t uart_ws_session;   // the /uart URI's user_ctx and the ctx of every /uart session

// httpd's free_ctx of every /ws and /uart session, called in the httpd task after the socket
// was closed. A /ws session's lane cursors stop there; the /uart tag is ignored.
static void websocket_session_ctx_free(void* ctx)
{
    ws_state_ctx_free(ctx);
}

static bool uart_websocket_client_connected(httpd_handle_t server)
{
    int client_fds[MAX_CLIENT];
    size_t clients = MAX_CLIENT;

    if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK)
        return false;

    for (size_t i = 0; i < clients; ++i)
    {
        if (httpd_sess_get_ctx(server, client_fds[i]) == &uart_ws_session && !ws_state_blocked(client_fds[i]) &&
            httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            return true;
    }

    return false;
}

static void wake_status_sender(void)
{
    TaskHandle_t task = status_sender_task;
    if (task)
        xTaskNotifyGive(task);
}

// Closes a session that ws.c gave up on. A close that could not be queued is retried by a state
// pass. One that was queued is never queued again: httpd queues a close by session slot, so a
// second one could close a new session that reuses the slot.
static void ws_close_session(httpd_handle_t server, int fd)
{
    esp_err_t err = httpd_sess_trigger_close(server, fd);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND)
        return; // queued, or the session is gone
    websocket_close_failures++;
    ws_state_close_failed(fd);
    ws_state_request_pass();
    wake_status_sender();
}

// A write failed: write nothing more to this session and close it. The blocked flag is set
// before the close is triggered, so no task sees the session as writable again until a new
// WebSocket handshake on its fd.
static void ws_session_failed(httpd_handle_t server, int fd, const char* name, esp_err_t err)
{
    websocket_send_failures++;
    ws_state_block(fd);
    ESP_LOGW(TAG, "%s: send failed for fd %d: %s", name, fd, esp_err_to_name(err));
    ws_close_session(server, fd);
}

static esp_err_t ws_send_frame(httpd_handle_t server, int fd, uint8_t* data, size_t len, const char* name)
{
    httpd_ws_frame_t frame = {
        .payload = data,
        .len = len,
        .type = HTTPD_WS_TYPE_BINARY,
    };
    esp_err_t err = httpd_ws_send_frame_async(server, fd, &frame);
    if (err != ESP_OK)
        ws_session_failed(server, fd, name, err);
    else
        ws_state_write_done(fd);
    return err;
}

typedef enum
{
    WS_READY,
    WS_NOT_WRITABLE,
    WS_HEAP_SHORT,
} ws_ready_t;

// A zero-timeout select returns before it allocates anything. A dead socket (RST) reads as
// writable, and its write fails at once.
static bool ws_fd_writable(int fd)
{
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval zero = {0, 0};
    return lwip_select(fd + 1, NULL, &w, NULL, &zero) > 0 && FD_ISSET(fd, &w);
}

// HTTPS only. Never heap_caps_get_largest_free_block() here: it walks the whole heap with
// interrupts masked, long enough to overflow the UART FIFO at 1.5 Mbaud. Free size and a TLSF
// malloc/free are O(1). The probe is fresh for every write, so the pbufs that earlier writes
// pinned are already counted.
static bool ws_tls_heap_ok(size_t need)
{
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) >= WS_HEAP_PROBE_GATE)
        return true;
    // The caps mbedTLS allocates with (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC).
    void* p = heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool ok = p != NULL;
    free(p);
    return ok;
}

// need: WS_TLS_WRITE_MIN_BLOCK for the status stream and the state pass, WS_TLS_UART_MIN_BLOCK
// for /uart.
static ws_ready_t ws_write_ready(int fd, bool https, size_t need)
{
    if (!ws_fd_writable(fd))
        return WS_NOT_WRITABLE;
    if (https && !ws_tls_heap_ok(need))
        return WS_HEAP_SHORT;
    return WS_READY;
}

static void count_write_skip(ws_ready_t ready)
{
    if (ready == WS_HEAP_SHORT)
        websocket_heap_skips++;
    else
        websocket_not_ready_skips++;
}

static void stall_close(httpd_handle_t server, int fd, bool heap)
{
    websocket_stall_closes++;
    ws_state_block(fd);
    ESP_LOGW(TAG, "fd %d not ready for %d s (%s): closing it", fd, WS_STALL_CLOSE_MS / 1000,
             heap ? "TLS heap" : "not writable");
    ws_close_session(server, fd);
}

// A write to fd was skipped as not ready. A WebSocket that stays so for WS_STALL_CLOSE_MS is
// closed: a socket that cannot take TCP_SNDLOWAT bytes in that time is dead in practice. A heap
// shortage is global, so of the sessions stalled for heap only, one is closed per
// WS_HEAP_CLOSE_GAP_MS: each close frees a TLS session (~4 KB), and the next pass re-checks.
static void ws_write_skipped(httpd_handle_t server, int fd, ws_ready_t ready)
{
    // Heap stalls happen only over HTTPS, so these are used by the httpd task only.
    static TickType_t last_heap_close;
    static bool have_heap_close;
    TickType_t now = xTaskGetTickCount();
    bool heap;

    count_write_skip(ready);
    ws_state_write_skipped(fd, now, ready == WS_HEAP_SHORT);
    if (!ws_state_stall_expired(fd, now, &heap))
        return;
    if (!heap)
    {
        stall_close(server, fd, false);
    }
    else if (!have_heap_close || now - last_heap_close >= pdMS_TO_TICKS(WS_HEAP_CLOSE_GAP_MS))
    {
        stall_close(server, fd, true);
        last_heap_close = now;
        have_heap_close = true;
    }
}

// httpd closes the least recently used session when a new connection arrives and all are
// taken. A WebSocket only counts as used when the client sends something, which the status
// and UART streams almost never do, so a page load would close a live stream before an idle
// keep-alive connection. The Wi-Fi status tick runs a status pass at least every 5 s (and
// every sensor sample one while a /ws session is open), and the pass marks every live
// WebSocket, the UART ones included, as used once a second. One whose writes have been
// skipped for WS_LRU_STALL_MS and that is still not writable is not kept ahead of idle
// connections. Writability alone is no test: a busy console is often unwritable for a moment,
// and a quiet one keeps the stall of its last skipped chunk.
// Called from one task in each mode: the status sender over HTTP, the httpd task over HTTPS.
static void ws_refresh_lru(httpd_handle_t server, const int* fds, size_t n)
{
    static TickType_t last_lru_update;
    TickType_t now = xTaskGetTickCount();
    if (last_lru_update != 0 && now - last_lru_update < pdMS_TO_TICKS(WS_LRU_UPDATE_INTERVAL_MS))
        return;

    for (size_t i = 0; i < n; ++i)
    {
        int fd = fds[i];
        void* ctx = httpd_sess_get_ctx(server, fd);
        if ((ws_state_ctx_is(fd, ctx) || ctx == &uart_ws_session) && !ws_state_blocked(fd) &&
            httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET &&
            (!ws_state_stalled_for(fd, now, pdMS_TO_TICKS(WS_LRU_STALL_MS)) || ws_fd_writable(fd)))
            httpd_sess_update_lru_counter(server, fd);
    }
    last_lru_update = now;
}

/** Sends one UART chunk to every /uart WebSocket. (The status stream is written by the status
 *  pass, ws_state_pass.) */
static void ws_broadcast(const struct ws_sender_config* config, const struct ws_message* msg)
{
    // Used from one task: the UART sender in HTTP mode, the httpd task in HTTPS mode.
    httpd_handle_t server = config->server;
    int client_fds[MAX_CLIENT];
    size_t clients = MAX_CLIENT;
    if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK)
        return;

    for (size_t i = 0; i < clients; ++i)
    {
        int fd = client_fds[i];
        if (httpd_sess_get_ctx(server, fd) != &uart_ws_session || ws_state_blocked(fd) ||
            httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;
        // /uart over HTTP is not checked: see "Never block on a WebSocket write".
        ws_ready_t ready = WS_READY;
        if (ws_https)
            ready = ws_write_ready(fd, ws_https, WS_TLS_UART_MIN_BLOCK);
        if (ready != WS_READY)
        {
            // Lost for this client only.
            ws_write_skipped(server, fd, ready);
            uart_queue_drops++;
            continue;
        }
        ws_send_frame(server, fd, msg->data, msg->len, config->name);
    }
}

// HTTPS: a TLS session must not be written by one task while httpd reads it in another,
// so the UART chunks go to the httpd task, a few at a time. (The status stream is one work
// item, ws_state_work, so a console flood cannot crowd it out.)
#define WS_HTTPS_MAX_PENDING 3

struct ws_work
{
    struct ws_sender_config* config;
    struct ws_message msg;
};

static void ws_broadcast_work(void* arg)
{
    struct ws_work* work = arg;
    ws_broadcast(work->config, &work->msg);
    free(work->msg.data);
    atomic_fetch_sub(&work->config->pending, 1);
    free(work);
}

typedef enum
{
    WS_QUEUED,
    WS_PENDING_FULL, // WS_HTTPS_MAX_PENDING frames are already pending
    WS_NO_MEMORY,    // the work item could not be allocated
    WS_CTRL_FAILED,  // httpd_queue_work failed
} ws_queue_result_t;

static ws_queue_result_t ws_queue_to_httpd(struct ws_sender_config* config, struct ws_message* msg)
{
    if (atomic_fetch_add(&config->pending, 1) >= WS_HTTPS_MAX_PENDING)
    {
        atomic_fetch_sub(&config->pending, 1);
        return WS_PENDING_FULL;
    }
    ws_queue_result_t result = WS_NO_MEMORY;
    struct ws_work* work = malloc(sizeof(*work));
    if (work)
    {
        work->config = config;
        work->msg = *msg;
        if (httpd_queue_work(config->server, ws_broadcast_work, work) == ESP_OK)
            return WS_QUEUED;
        free(work);
        result = WS_CTRL_FAILED;
    }
    atomic_fetch_sub(&config->pending, 1);
    return result;
}

// Sends one dequeued UART chunk (HTTP) or hands it to the httpd task (HTTPS), and frees it.
static void ws_sender_handle(struct ws_sender_config* config, struct ws_message* msg, bool https)
{
    if (!https)
    {
        ws_broadcast(config, msg);
        free(msg->data);
        return;
    }
    if (ws_queue_to_httpd(config, msg) == WS_QUEUED)
        return;
    uart_queue_drops++;
    free(msg->data);
}

// The UART stream's sender.
static void ws_sender_task(void* arg)
{
    struct ws_sender_config* config = arg;
    const bool https = webserver_is_https();
    struct ws_message msg;

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

        ws_sender_handle(config, &msg, https);
    }
}

// ---- The status stream: state, events and sensor samples ---------------------------------
// sw_status and wifi_status are latest values (ws_state.c), encoded when their frame is
// written. Events and sensor samples are held in ws_state.c's lanes until every open /ws
// session got them, each session from its own cursors: only an event the ring evicted before
// a session got it is lost to that session, which is then sent a marker counting them. A
// status pass writes every /ws session what it is owed. One task writes the stream, its
// cursors and marks in each mode: the status sender over HTTP, the httpd task over HTTPS (one
// work item, kicked by the sender).

#define WS_KICK_RETRY_MS 20    // HTTPS: queuing the state pass failed, try again after this
#define WS_KICK_REARM_MS 10000 // HTTPS: a queued state pass has not run: its datagram was lost

void ws_publish_switch_state(void)
{
    ws_state_bump_sw();
    ws_state_request_pass();
    wake_status_sender();
}

void ws_publish_wifi_status(const uint8_t* frame, size_t len)
{
    if (!ws_state_store_wifi(frame, len))
    {
        ESP_LOGW(TAG, "wifi status frame of %u B not stored", (unsigned)len);
        return;
    }
    ws_state_request_pass();
    wake_status_sender();
}

// First byte of an encoded StatusMessage: the tag of its oneof payload.
#define PB_TAG_SENSOR 0x0a // StatusMessage.sensor_data, field 1, length-delimited
#define PB_TAG_EVENT 0x2a  // StatusMessage.event_data, field 5
_Static_assert(PB_TAG_SENSOR == ((StatusMessage_sensor_data_tag << 3) | PB_WT_STRING), "proto renumbered");
_Static_assert(PB_TAG_EVENT == ((StatusMessage_event_data_tag << 3) | PB_WT_STRING), "proto renumbered");
_Static_assert(WS_EVENT_FRAME_MAX == PB_BUFFER_SIZE, "the event lane must take every send_pb_message frame");
_Static_assert(SensorData_size < 128 && WS_SENSOR_FRAME_MAX == 2 + SensorData_size, "sensor frame size");
#define WS_MARKER_FRAME_MAX 80 // 77 B: 2a 4b | 08 01 | 10 +10 | 18 +10 | 22 31 + 49 chars
#define WS_BATCHES_PER_FD 8    // writes per session and pass; a longer backlog gets another pass

// What one write to a session carries, and what it moves once it was accepted whole.
struct ws_batch_plan
{
    uint32_t epoch;           // the session it is built for (ws_state_owed)
    bool lane_ok;             // the lane view is of that session
    struct ws_lane_view lane; // what it is built from
    uint32_t ev_next;         // the event cursor after it
    uint32_t sensor_next;     // the sensor cursor after it
    uint32_t sensor_merged;   // samples skipped: more were pending than held, or overwritten
    bool marker;              // it carries the marker for lane.ev_missed events
    bool sw;                  // it carries sw_status of sw_gen
    bool wifi;                // it carries wifi_status of wifi_gen
    uint32_t sw_gen;
    uint32_t wifi_gen;
    unsigned events;  // event frames it carries
    unsigned sensors; // sensor frames it carries
};

// The one writer of each mode only: the status sender over HTTP, the httpd task over HTTPS.
static uint8_t status_frame[PB_BUFFER_SIZE];

// The marker a session gets ahead of its events when it missed n of them: an EventData at
// warning level, stamped now (the times of the lost events are unknown).
static size_t ws_encode_marker(uint8_t* buf, size_t size, uint32_t n)
{
    char text[64];
    snprintf(text, sizeof(text), "status link: %" PRIu32 " event%s not delivered", n, n == 1 ? " was" : "s were");
    return encode_event(buf, size, EV_WARNING, text);
}

// One state frame: sw_status if owed, otherwise wifi_status. Returns its length, or 0.
static size_t ws_build_state_frame(const struct ws_owed* owed, uint8_t* buf, size_t size, struct ws_batch_plan* plan)
{
    uint32_t gen;
    size_t len;
    if (owed->sw)
    {
        gen = ws_state_sw_gen();                    // the generation first,
        len = encode_load_switch_status(buf, size); // then the outputs
        if (len)
        {
            plan->sw = true;
            plan->sw_gen = gen;
        }
        return len;
    }
    len = ws_state_copy_wifi(buf, size, &gen); // bytes and generation as one pair
    if (len)
    {
        plan->wifi = true;
        plan->wifi_gen = gen;
    }
    return len;
}

// The next StatusMessage the session on fd is owed, into buf. A new session (owed->snapshot)
// gets its state first. Otherwise the order is: events (after a marker if some were missed),
// then state, then sensor samples. Returns its length, or 0 when nothing can be written now:
// the session ended or was replaced since the owed read, a producer evicted or overwrote what
// the view held, or an event was stored after the view (the state then waits behind it).
static size_t ws_build_frame(int fd, const struct ws_owed* owed, uint8_t* buf, size_t size, struct ws_batch_plan* plan)
{
    *plan = (struct ws_batch_plan){.epoch = owed->epoch};
    plan->lane_ok = ws_state_lane_view(fd, &plan->lane) && plan->lane.epoch == owed->epoch;
    if (!plan->lane_ok)
        return 0; // not even owed state: the fd may hold another session by now
    const struct ws_lane_view* lane = &plan->lane;
    plan->ev_next = lane->ev_cursor;
    plan->sensor_next = lane->sensor_from;
    plan->sensor_merged = lane->sensor_merged;
    if (owed->snapshot)
        return ws_build_state_frame(owed, buf, size, plan);
    size_t len;
    if (lane->ev_missed)
    {
        len = ws_encode_marker(buf, size, lane->ev_missed);
        if (len)
        {
            plan->marker = true;
            plan->ev_next = lane->ev_from;
        }
        return len;
    }
    if (lane->ev_from != lane->ev_end)
    {
        bool gone;
        len = ws_state_copy_event(lane->ev_from, buf, size, &gone); // 0: evicted since the view
        if (len)
        {
            plan->ev_next = lane->ev_from + 1;
            plan->events = 1;
        }
        return len;
    }
    if (owed->sw || owed->wifi)
    {
        len = ws_build_state_frame(owed, buf, size, plan);
        // An event stored after the view may have been stored before the state was read (the
        // warning task pushes "warning action", then cuts the outputs): it goes first.
        if (len && ws_state_ev_head() != lane->ev_end)
        {
            plan->sw = plan->wifi = false;
            return 0;
        }
        return len;
    }
    for (uint32_t s = lane->sensor_from; s != lane->sensor_end; ++s)
    {
        plan->sensor_next = s + 1;
        len = ws_state_copy_sensor(s, buf, size);
        if (len)
        {
            plan->sensors = 1;
            return len;
        }
        plan->sensor_merged++; // overwritten since the view
    }
    return 0;
}

// The write that plan describes was accepted whole: move the cursors and mark the state. All of
// it is ignored if the fd no longer holds the session it was built for.
static void ws_commit_batch(int fd, const struct ws_batch_plan* plan)
{
    ws_state_lane_commit(fd, plan->epoch, plan->ev_next, plan->marker ? plan->lane.ev_missed : 0, plan->sensor_next,
                         plan->sensor_merged);
    if (plan->sw)
    {
        ws_state_mark_sw(fd, plan->epoch, plan->sw_gen); // never a generation read after the outputs
        state_frames++;
    }
    if (plan->wifi)
    {
        ws_state_mark_wifi(fd, plan->epoch, plan->wifi_gen);
        state_frames++;
    }
}

typedef enum
{
    WS_SEND_DONE,      // nothing more is owed
    WS_SEND_NOT_READY, // owed, but nothing could be written now: try again soon
    WS_SEND_FAILED,    // a write failed: the session is blocked and being closed
    WS_SEND_MORE,      // WS_BATCHES_PER_FD writes went out and more is owed
} ws_send_t;

// Writes the session on fd what it is owed, one readiness check per write. Called only by the
// one writer of the current mode, for an fd whose ctx is its /ws context (ws_state_ctx), not
// blocked, and a WebSocket.
static ws_send_t ws_send_owed(httpd_handle_t server, int fd)
{
    bool snapshot = false; // a new session's sw_status and wifi_status go out ahead of the rest
    for (int b = 0; b < WS_BATCHES_PER_FD; ++b)
    {
        struct ws_owed owed;
        if (!ws_state_owed(fd, &owed))
            return WS_SEND_DONE;
        snapshot = (snapshot || owed.snapshot) && (owed.sw || owed.wifi);
        owed.snapshot = snapshot;
        ws_ready_t ready = ws_write_ready(fd, ws_https, WS_TLS_WRITE_MIN_BLOCK);
        if (ready != WS_READY)
        {
            ws_write_skipped(server, fd, ready); // stays owed, cursors untouched
            return WS_SEND_NOT_READY;
        }
        struct ws_batch_plan plan;
        size_t len = ws_build_frame(fd, &owed, status_frame, sizeof(status_frame), &plan);
        if (len == 0)
            return WS_SEND_NOT_READY; // nothing written: the 200 ms re-poll, or the producer's
                                      // request, runs the next pass. Never spins.
        if (ws_send_frame(server, fd, status_frame, len, status_sender.name) != ESP_OK)
            return WS_SEND_FAILED; // its free_ctx counts what it did not get
        ws_commit_batch(fd, &plan);
    }
    struct ws_owed left; // reached only after WS_BATCHES_PER_FD writes: progress was made
    return ws_state_owed(fd, &left) ? WS_SEND_MORE : WS_SEND_DONE;
}

// The smaller non-zero interval, or 0 when both are 0.
static uint32_t min_nonzero(uint32_t a, uint32_t b)
{
    if (a == 0)
        return b;
    if (b == 0)
        return a;
    return a < b ? a : b;
}

// The status pass: writes every registered /ws session what it is owed, and retries the closes
// that could not be queued. Returns the re-poll interval in ms (0: none). Runs in the status
// sender over HTTP, in the httpd task over HTTPS.
static uint32_t ws_state_pass(httpd_handle_t server)
{
    int client_fds[MAX_CLIENT];
    size_t clients = MAX_CLIENT;
    if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK)
        return 0;

    ws_refresh_lru(server, client_fds, clients);
    uint32_t repoll = 0;
    unsigned owed = 0;
    bool more = false;
    for (size_t i = 0; i < clients; ++i)
    {
        int fd = client_fds[i];
        if (!ws_state_slot(fd, NULL))
        {
            // lwIP hands out no such fd. It is never written (it reads as blocked).
            static bool logged;
            if (!logged)
                ESP_LOGE(TAG, "fd %d is outside the WebSocket state table", fd);
            logged = true;
            continue;
        }
        if (ws_state_take_close_retry(fd)) // one claimer
        {
            // Only while the fd still holds that session: one of ours, blocked, and an open
            // WebSocket. After the client's CLOSE httpd closes the session itself, and a second
            // queued close could close a new session in the same slot.
            void* ctx = httpd_sess_get_ctx(server, fd);
            if ((ws_state_ctx_is(fd, ctx) || ctx == &uart_ws_session) && ws_state_blocked(fd) &&
                httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET)
            {
                websocket_close_retries++;
                esp_err_t err = httpd_sess_trigger_close(server, fd);
                if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
                {
                    ws_state_close_failed(fd);
                    repoll = min_nonzero(repoll, WS_NOT_READY_REPOLL_MS);
                }
            }
            continue;
        }
        if (!ws_state_ctx_is(fd, httpd_sess_get_ctx(server, fd)) || ws_state_blocked(fd))
            continue;
        if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue; // not upgraded yet, or closing (the client sent CLOSE)
        switch (ws_send_owed(server, fd))
        {
        case WS_SEND_NOT_READY:
            repoll = min_nonzero(repoll, WS_NOT_READY_REPOLL_MS); // stays owed: try again soon
            break;
        case WS_SEND_MORE:
            more = true;
            break;
        case WS_SEND_DONE:
        case WS_SEND_FAILED: // the session is blocked and already being closed
            break;
        }
        struct ws_owed left;
        if (ws_state_owed(fd, &left) && (left.sw || left.wifi) && !ws_state_blocked(fd))
            owed++; // status_state_owed counts sessions owed state only
    }
    state_owed = owed;
    if (more)
    {
        // A backlog longer than WS_BATCHES_PER_FD writes: the next pass goes behind whatever
        // else is queued (over HTTPS, httpd's other work items).
        ws_state_request_pass();
        wake_status_sender();
    }
    return repoll;
}

// HTTPS: the status pass, in the httpd task. Static and without an argument, so a duplicate is
// harmless.
static void ws_state_work(void* arg)
{
    (void)arg;
    ws_state_pass_started(); // first: a request from now on needs a new kick
    ws_state_report_pass(ws_state_pass(status_sender.server));
    wake_status_sender();
}

static bool queue_state_work(void* server)
{
    return httpd_queue_work(server, ws_state_work, NULL) == ESP_OK;
}

// Wrap-safe: deadlines are at most WS_KICK_REARM_MS ahead.
static bool due(TickType_t now, TickType_t at)
{
    return (int32_t)(now - at) >= 0;
}

static TickType_t ticks_until(TickType_t now, TickType_t at)
{
    return due(now, at) ? 0 : at - now;
}

static TickType_t min_ticks(TickType_t a, TickType_t b)
{
    return a < b ? a : b;
}

// The status stream's sender: runs the status pass over HTTP, kicks it over HTTPS. Woken by
// task notifications from the publishers, push_data_to_ws and the work item.
static void ws_status_sender_task(void* arg)
{
    struct ws_sender_config* config = arg;
    const bool https = webserver_is_https();
    bool repoll = false; // run a pass (HTTP) or kick one (HTTPS) at repoll_at
    TickType_t repoll_at = 0;
    bool kick_retry = false; // HTTPS: queuing failed, kick again at kick_retry_at
    TickType_t kick_retry_at = 0;
    TickType_t kicked_at = 0; // HTTPS: when the outstanding kick was queued

    for (;;)
    {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = portMAX_DELAY;
        if (repoll)
            wait = min_ticks(wait, ticks_until(now, repoll_at));
        if (kick_retry)
            wait = min_ticks(wait, ticks_until(now, kick_retry_at));
        if (https && ws_state_kick_outstanding())
            wait = min_ticks(wait, ticks_until(now, kicked_at + pdMS_TO_TICKS(WS_KICK_REARM_MS)));
        ulTaskNotifyTake(pdTRUE, wait);
        now = xTaskGetTickCount();

        bool need = ws_state_take_pass_request();
        uint32_t ms;
        if (https && ws_state_take_report(&ms))
        {
            repoll = ms != 0;
            repoll_at = now + pdMS_TO_TICKS(ms);
        }
        if (repoll && due(now, repoll_at))
        {
            repoll = false;
            need = true;
        }
        if (https)
        {
            if (ws_state_kick_outstanding() && now - kicked_at >= pdMS_TO_TICKS(WS_KICK_REARM_MS))
            {
                state_kick_rearms++; // its control datagram was lost
                ws_state_kick_reset();
                need = true;
            }
            if (kick_retry && due(now, kick_retry_at))
            {
                kick_retry = false;
                need = true;
            }
            if (need)
            {
                switch (ws_state_kick(queue_state_work, config->server))
                {
                case WS_KICK_QUEUED:
                    kicked_at = now;
                    break;
                case WS_KICK_OUTSTANDING:
                    break; // the queued pass has not started: it reads the new state
                case WS_KICK_FAILED:
                    state_kick_failures++;
                    kick_retry = true;
                    kick_retry_at = now + pdMS_TO_TICKS(WS_KICK_RETRY_MS);
                    break;
                }
            }
        }
        else if (need)
        {
            ms = ws_state_pass(config->server);
            repoll = ms != 0;
            repoll_at = now + pdMS_TO_TICKS(ms);
        }
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

// upgrade: the WebSocket handshake (pre-handshake callback); otherwise a plain GET of the URI.
static esp_err_t websocket_handshake(httpd_req_t* req, bool uart_stream, bool upgrade)
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
    // /uart sessions share one tag. A /ws session's context is its fd's state entry, so that
    // free_ctx, which gets no fd, can tell which session ended.
    void* ctx = uart_stream ? (void*)&uart_ws_session : ws_state_ctx(fd);
    if (!ctx)
    {
        httpd_resp_send_500(req); // lwIP hands out no such fd (see MAX_CLIENT)
        return ESP_FAIL;
    }
    // A new WebSocket session on this fd: everything the previous one left is reset at once. A
    // /ws session is open from here to its free_ctx, and gets the events stored from now on.
    if (upgrade)
        ws_state_session_opened(fd, !uart_stream);
    else
        ws_state_unblock(fd);
    req->sess_ctx = ctx;
    req->free_ctx = websocket_session_ctx_free; // never NULL: httpd would free() the context

    return ESP_OK;
}

static esp_err_t ws_pre_handshake_cb(httpd_req_t* req)
{
    return websocket_handshake(req, req->user_ctx == &uart_ws_session, true);
}

// /ws only, in the httpd task right after ws_handshake_done is set: any pass from now on sees
// this fd as a WebSocket, and the session gets sw_status and wifi_status first. httpd fails
// the request unless this returns ESP_OK.
static esp_err_t ws_post_handshake_cb(httpd_req_t* req)
{
    (void)req;
    ws_state_request_pass();
    wake_status_sender();
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET)
        return websocket_handshake(req, false, false);

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
        return websocket_handshake(req, true, false);

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
    char buf[16];
    long baud_rate = 0;
    if (nconfig_read(UART_BAUD_RATE, buf, sizeof(buf)) == ESP_OK)
        baud_rate = strtol(buf, NULL, 10);

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    // A bad stored rate must not abort boot, or the device can only be recovered by reflashing.
    if (!uart_baud_rate_is_supported(baud_rate) || uart_param_config(UART_NUM, &uart_config) != ESP_OK)
    {
        ESP_LOGW(TAG, "Invalid stored baud rate, falling back to %d", UART_DEFAULT_BAUD_RATE);
        uart_config.baud_rate = UART_DEFAULT_BAUD_RATE;
        snprintf(buf, sizeof(buf), "%d", UART_DEFAULT_BAUD_RATE);
        nconfig_write(UART_BAUD_RATE, buf);
        ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    }
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode(UART_RX_PIN, GPIO_PULLUP_ONLY);
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_RX_BUFFER_SIZE, UART_TX_BUFFER_SIZE, 20, &uart_event_queue, 0));
    ws_https = webserver_is_https();

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = &status_ws_session,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake_cb,
        .ws_post_handshake_cb = ws_post_handshake_cb,
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

    uart_ws_queue = xQueueCreate(UART_WS_QUEUE_LENGTH, sizeof(struct ws_message));
    // No status queue: events and sensor samples are held in ws_state.c's lanes.
    status_sender = (struct ws_sender_config){
        .queue = NULL, .server = server, .uart_stream = false, .name = "ws-status"};
    uart_sender = (struct ws_sender_config){
        .queue = uart_ws_queue, .server = server, .uart_stream = true, .name = "ws-uart"};
    // Stack sizes leave 1.3-2.3 KB above the deepest use measured while page loads, both
    // streams and an SBC boot log were running and send failures were logged
    // (sender ~1.7 KB, polling ~0.7 KB).
    xTaskCreate(uart_polling_task, "uart_polling_task", 1024 * 3, server, 8, NULL);
    TaskHandle_t task = NULL;
    xTaskCreate(ws_status_sender_task, "ws_status_sender", 1024 * 3, &status_sender, 9, &task);
    status_sender_task = task;
    xTaskCreate(ws_sender_task, "ws_uart_sender", 1024 * 3, &uart_sender, 9, NULL);
    xTaskCreate(uart_event_task, "uart_event_task", 1024 * 2, NULL, 10, NULL);
}

void push_data_to_ws(const uint8_t* data, size_t len)
{
    if (len == 0)
        return;
    // Any tag other than sensor_data is kept in order with the events. No other exists:
    // sw_status and wifi_status go through ws_publish_*.
    enum ws_push r = data[0] == PB_TAG_SENSOR ? ws_state_push_sensor(data, len) : ws_state_push_event(data, len);
    if (r == WS_PUSH_REFUSED)
    {
        ESP_LOGW(TAG, "status frame of %u B not stored", (unsigned)len); // cannot happen: see the asserts
        return;
    }
    if (r == WS_PUSH_NO_SESSION)
        return; // nobody to send it to; a new session's cursors start after it
    ws_state_request_pass();
    wake_status_sender();
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
    diagnostics->websocket_close_failures = websocket_close_failures;
    diagnostics->websocket_close_retries = websocket_close_retries;
    diagnostics->websocket_not_ready_skips = websocket_not_ready_skips;
    diagnostics->websocket_heap_skips = websocket_heap_skips;
    diagnostics->websocket_stall_closes = websocket_stall_closes;
    diagnostics->status_drops_queue_full = status_drops_queue_full;
    diagnostics->status_drops_https_pending = status_drops_https_pending;
    diagnostics->status_drops_ctrl_queue = status_drops_ctrl_queue;
    diagnostics->status_drops_no_memory = status_drops_no_memory;
    struct ws_lane_stats lanes;
    ws_state_lane_stats(&lanes);
    diagnostics->status_drops_no_client = lanes.ev_no_session;
    diagnostics->status_sw_drops = status_sw_drops;
    diagnostics->status_https_pending = atomic_load(&status_sender.pending);
    diagnostics->uart_https_pending = atomic_load(&uart_sender.pending);
    diagnostics->status_state_frames = state_frames;
    diagnostics->status_state_owed = state_owed;
    diagnostics->status_state_kick_failures = state_kick_failures;
    diagnostics->status_state_kick_rearms = state_kick_rearms;
}

bool uart_baud_rate_is_supported(long baud_rate)
{
    for (size_t i = 0; i < sizeof(supported_baud_rates) / sizeof(supported_baud_rates[0]); ++i)
    {
        if (supported_baud_rates[i] == baud_rate)
            return true;
    }
    return false;
}

esp_err_t change_baud_rate(int baud_rate)
{
    return uart_set_baudrate(UART_NUM, baud_rate);
}
