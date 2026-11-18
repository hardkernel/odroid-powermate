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
#define UART_MIN_FREE_HEAP (48 * 1024) // below this much free heap /uart holds (never drops)
// /uart: see "the bridge and the drain" below.
#define UART_FRAME_MAX 1400             // payload: 4 + 1400 + 29 B of TLS 1.2 GCM = one 1440 B MSS
#define UART_FRAME_HEAD 4               // room for the longest header this payload needs
#define UART_WAKE_BYTES 1024            // a frame is due at this much buffered,
#define UART_WAKE_AGE_MS 8              // or once the oldest byte has waited this long,
#define UART_IDLE_MS 10                 // or at once if no frame was taken for this long
#define UART_DRAIN_FRAMES 8             // frames per drain run (~11 KB)
#define UART_HOLD_RETRY_MS 10           // a held drain runs again after this
#define UART_HOLD_RING_MAX (10 * 1024)  // a not-ready viewer holds the ready ones while the ring is below this
#define UART_HOLD_MS_MAX 200            // and the hold is younger than this; then it is marked lagging
#define UART_LAG_REGATE_FRAMES 8        // a lagging viewer gates again after this many ready frames
#define UART_LAG_REGATE_RING (4 * 1024) // in a row with the ring below this
#define UART_SHED_HIGH (14 * 1024)      // the bridge sheds the oldest bytes from this
#define UART_SHED_LOW (12 * 1024)       // down to this
#define UART_SHED_STALL_MS 10           // when no frame was taken for this long
#define UART_KICK_RETRY_MS 20           // HTTPS: httpd_queue_work failed, kick again after this
#define UART_KICK_REARM_MS 1000         // HTTPS: a queued drain has not started: its datagram was lost
#define UART_KICK_FALLBACK_MS 2000      // HTTPS: two queued drains have not started: both were lost
#define UART_BRIDGE_IDLE_MS 20          // the bridge re-checks an empty ring at least this often
#define UART_WRITER_IDLE_MS 100         // HTTP: the writer runs at least this often
#define UART_SCRATCH_SIZE 256           // the bridge's discard buffer
// Stacks: the bridge calls lwIP sendto (the kick), uart_read_bytes and ESP_LOGW; the HTTP writer
// an lwIP send and ESP_LOGW (v1.x's UART sender, which did the same, peaked at ~1.7 KB).
#define UART_BRIDGE_STACK 4096
#define UART_WRITER_STACK 3584
#define UART_BRIDGE_PRIO_HTTPS 12 // = httpd: time-sliced every tick, even during a handshake
#define UART_BRIDGE_PRIO_HTTP 10  // below httpd, above the writer
#define UART_WRITER_PRIO 9        // HTTP: below httpd, so it cannot split httpd's two-write PONG
_Static_assert(UART_SHED_LOW < UART_SHED_HIGH && UART_SHED_HIGH < UART_RX_BUFFER_SIZE, "shed band outside the ring");
_Static_assert(UART_HOLD_RING_MAX < UART_SHED_LOW, "a hold must end before the bridge sheds");
_Static_assert(UART_FRAME_MAX >= UART_WAKE_BYTES, "a due frame must fit one read");
_Static_assert(UART_FRAME_MAX >= 126 && UART_FRAME_MAX < 65536, "UART_FRAME_HEAD assumes a 16-bit length");
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
// samples older than the newest WS_SENSOR_SLOTS. On /uart the bytes wait in the UART ring while
// a viewer is not ready (see uart_drain); a viewer that stays so past the hold cap loses frames
// (counted) until it catches up, so one slow or dead viewer cannot stall the others.
// The price over HTTP: a /uart socket keeps at most TCP_SNDLOWAT + one frame unacknowledged
// (~7 KB) instead of the whole send buffer, so a viewer whose ACKs take longer than ~50 ms
// cannot keep up with a full-rate flood at 1.5 Mbaud (over HTTPS this was always so).

#define WS_FRAME_HEADER_MAX 10         // httpd_ws.c: 2 B + 8 B extended length (server frames are not masked)
#define WS_TLS_RECORD_OVERHEAD_MAX 128 // TLS 1.2 header + explicit IV + MAC + padding, upper bound
// One readiness check covers one status write (one httpd_socket_send of whole frames, so one TLS
// record), or one UART frame (header and payload in one write, one TLS record).
#define WS_BATCH_SIZE 384
#define WS_STATUS_WRITE_MAX (WS_BATCH_SIZE + WS_TLS_RECORD_OVERHEAD_MAX) // 512
#define WS_UART_WRITE_MAX (UART_FRAME_HEAD + UART_FRAME_MAX + WS_TLS_RECORD_OVERHEAD_MAX)
#define WS_TLS12_GCM_OVERHEAD 29 // 5 B header + 8 B explicit nonce + 16 B tag
_Static_assert(WS_STATUS_WRITE_MAX < TCP_SNDLOWAT, "a status write after a readiness check could block");
_Static_assert(WS_UART_WRITE_MAX < TCP_SNDLOWAT, "a UART write after a readiness check could block");
// 2 segments for one status write, 1 for a UART frame (2 with a CBC suite).
_Static_assert(TCP_SNDQUEUELOWAT - 1 + 6 <= TCP_SND_QUEUELEN, "the segment queue could fill after a check");
_Static_assert(UART_FRAME_HEAD + UART_FRAME_MAX + WS_TLS12_GCM_OVERHEAD <= TCP_MSS,
               "a UART frame is assumed to be one segment over HTTPS");
_Static_assert(UART_FRAME_HEAD + UART_FRAME_MAX <= CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN, "a UART frame must be one TLS record");

// Every TLS record write allocates a TX buffer of 8 B + MBEDTLS_SSL_OUT_BUFFER_LEN (13 header +
// 16 IV + 48 MAC + 256 padding + 4096 content). The records of one call pin up to two MSS pbufs
// until ACKed (Nagle: the first record goes out, the rest wait in a second segment). A UART
// frame pins fewer; its probe keeps the size of the 2 KB chunks it replaced.
#define WS_TLS_TX_BUF_LEN 4437
#define WS_PBUF_RESERVE 3072             // two MSS pbufs per status write
#define WS_UART_PBUF_RESERVE 4608        // three MSS pbufs per /uart fd (an upper bound)
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

struct ws_sender_config
{
    httpd_handle_t server;
    const char* name;
};

#define MAX_CLIENT POWERMATE_HTTP_MAX_OPEN_SOCKETS
// Every httpd session is an lwIP socket, and the per-fd state table (ws_state.c) has one slot
// per lwIP socket, so it covers every session.
_Static_assert(POWERMATE_HTTP_MAX_OPEN_SOCKETS + 3 <= CONFIG_LWIP_MAX_SOCKETS,
               "CONFIG_LWIP_MAX_SOCKETS too small for POWERMATE_HTTP_MAX_OPEN_SOCKETS: delete sdkconfig and rebuild");
static QueueHandle_t uart_event_queue;
static struct ws_sender_config status_sender;
static volatile uint32_t uart_received_bytes;
static volatile uint32_t uart_fifo_overflows;     // overflow episodes with a FIFO overrun
static volatile uint32_t uart_buffer_full_events; // overflow episodes with a full ring
static volatile uint32_t uart_queue_drops;        // shed events + (viewer, frame) losses
// Where every byte read out of the ring went: uart_received_bytes = uart_frame_bytes +
// uart_shed_bytes + uart_no_viewer_bytes, exactly whenever no read is in progress (each read
// is counted in both right after it returns).
static volatile uint32_t uart_frame_bytes;       // read into frames for the viewers
static volatile uint32_t uart_shed_bytes;        // discarded: the ring was nearly full and no frame went out
static volatile uint32_t uart_no_viewer_bytes;   // discarded: no /uart viewer
static volatile uint32_t uart_viewer_drop_bytes; // per viewer: frame bytes it did not get
static volatile uint32_t uart_lagging_marks;     // viewers that stopped holding the others
static volatile uint32_t uart_kick_failures;     // HTTPS: queuing the drain failed (retried)
static volatile uint32_t websocket_send_failures;
static volatile uint32_t websocket_close_failures; // httpd_sess_trigger_close failed (retried)
static volatile uint32_t websocket_close_retries;  // closes re-triggered by a state pass
static volatile uint32_t websocket_not_ready_skips; // writes skipped: the socket was not writable
static volatile uint32_t websocket_heap_skips;      // HTTPS writes skipped: no heap for the TLS record
static volatile uint32_t websocket_stall_closes;    // sessions closed after WS_STALL_CLOSE_MS not ready
static volatile uint32_t status_stall_closes;       // /ws sessions among them
static volatile uint32_t status_batches;            // status writes (one send, one TLS record each)
static bool ws_https; // webserver_is_https(), set by register_ws_endpoint
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

typedef enum
{
    UART_SEEN_NONE,    // no /uart viewer: the bridge may discard
    UART_SEEN_VIEWER,  // a /uart viewer
    UART_SEEN_UNKNOWN, // an open WebSocket without a session context: treat as a viewer
} uart_seen_t;

// The bridge's /uart viewer check. Over HTTPS the bridge runs at httpd's priority and can
// preempt httpd at a tick inside httpd_req_new, after it set the request's session
// (hd_req_aux.sd) and before it copied the session's context: httpd_sess_get_ctx then returns
// NULL for that session. The same holds on a close, between httpd_sess_clear_ctx and the slot
// being freed. Every WebSocket URI here sets its context in the handshake, so an open WebSocket
// with a NULL context is UNKNOWN for this pass, never "no viewer" (which would discard the
// whole ring). The drain's own check (uart_viewers) runs in httpd over HTTPS and below httpd
// over HTTP, so it never sees that state.
static uart_seen_t uart_websocket_client_connected(httpd_handle_t server)
{
    int client_fds[MAX_CLIENT];
    size_t clients = MAX_CLIENT;

    if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK)
        return UART_SEEN_NONE;

    uart_seen_t seen = UART_SEEN_NONE;
    for (size_t i = 0; i < clients; ++i)
    {
        if (ws_state_blocked(client_fds[i]) ||
            httpd_ws_get_fd_info(server, client_fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;
        void* ctx = httpd_sess_get_ctx(server, client_fds[i]);
        if (ctx == &uart_ws_session)
            return UART_SEEN_VIEWER;
        if (ctx == NULL)
            seen = UART_SEEN_UNKNOWN;
    }

    return seen;
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
    __atomic_fetch_add(&websocket_close_failures, 1, __ATOMIC_RELAXED); // two writers over HTTP
    ws_state_close_failed(fd);
    ws_state_request_pass();
    wake_status_sender();
}

// A write failed: write nothing more to this session and close it. The blocked flag is set
// before the close is triggered, so no task sees the session as writable again until a new
// WebSocket handshake on its fd.
static void ws_session_failed(httpd_handle_t server, int fd, const char* name, esp_err_t err)
{
    __atomic_fetch_add(&websocket_send_failures, 1, __ATOMIC_RELAXED); // two writers over HTTP
    ws_state_block(fd);
    ESP_LOGW(TAG, "%s: send failed for fd %d: %s", name, fd, esp_err_to_name(err));
    ws_close_session(server, fd);
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

// Over HTTP the status sender and the UART writer both count here.
static void count_write_skip(ws_ready_t ready)
{
    __atomic_fetch_add(ready == WS_HEAP_SHORT ? &websocket_heap_skips : &websocket_not_ready_skips, 1,
                       __ATOMIC_RELAXED);
}

static void stall_close(httpd_handle_t server, int fd, bool heap, bool status)
{
    __atomic_fetch_add(&websocket_stall_closes, 1, __ATOMIC_RELAXED); // two writers over HTTP
    if (status)
        status_stall_closes++;
    ws_state_block(fd);
    ESP_LOGW(TAG, "fd %d not ready for %d s (%s): closing it", fd, WS_STALL_CLOSE_MS / 1000,
             heap ? "TLS heap" : "not writable");
    ws_close_session(server, fd);
}

// fd was not ready for a write. A WebSocket that stays so for WS_STALL_CLOSE_MS is closed: a
// socket that cannot take TCP_SNDLOWAT bytes in that time is dead in practice. A heap shortage
// is global, so of the sessions stalled for heap only, one is closed per WS_HEAP_CLOSE_GAP_MS:
// each close frees a TLS session (~4 KB), and the next pass re-checks. Not counted as a skip:
// /uart calls this while it holds a frame for a not-ready viewer.
// status: fd is a /ws session (for the diagnostics only).
static void ws_stall_clock(httpd_handle_t server, int fd, ws_ready_t ready, bool status)
{
    // Heap stalls happen only over HTTPS, so these are used by the httpd task only.
    static TickType_t last_heap_close;
    static bool have_heap_close;
    TickType_t now = xTaskGetTickCount();
    bool heap;

    ws_state_write_skipped(fd, now, ready == WS_HEAP_SHORT);
    if (!ws_state_stall_expired(fd, now, &heap))
        return;
    if (!heap)
    {
        stall_close(server, fd, false, status);
    }
    else if (!have_heap_close || now - last_heap_close >= pdMS_TO_TICKS(WS_HEAP_CLOSE_GAP_MS))
    {
        stall_close(server, fd, true, status);
        last_heap_close = now;
        have_heap_close = true;
    }
}

// A write to fd was skipped as not ready: counted, and the stall clock runs.
static void ws_write_skipped(httpd_handle_t server, int fd, ws_ready_t ready, bool status)
{
    count_write_skip(ready);
    ws_stall_clock(server, fd, ready, status);
}

// httpd closes the least recently used session when a new connection arrives and all are
// taken. A WebSocket only counts as used when the client sends something, which the status
// and UART streams almost never do, so a page load would close a live stream before an idle
// keep-alive connection. The Wi-Fi status tick runs a status pass at least every 5 s (and
// every sensor sample one while a /ws session is open), and the pass marks every live
// WebSocket, the UART ones included, as used once a second. One whose writes have been
// skipped for WS_LRU_STALL_MS and that is still not writable is not kept ahead of idle
// connections. Writability alone is no test: a busy console is often unwritable for a moment,
// and a quiet one keeps the stall of its last held or skipped frame.
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
_Static_assert(WS_WIFI_FRAME_MAX < 126, "wifi_status takes a 2-byte WebSocket header");
#define WS_SW_FRAME_MAX 8      // sw_status is at most 6 B
#define WS_MARKER_FRAME_MAX 80 // 77 B: 2a 4b | 08 01 | 10 +10 | 18 +10 | 22 31 + 49 chars
#define WS_BATCHES_PER_FD 8    // writes per session and pass; a longer backlog gets another pass
_Static_assert(WS_BATCH_SIZE >= 4 + WS_EVENT_FRAME_MAX, "a batch must hold the largest frame");
_Static_assert(WS_BATCH_SIZE >= 2 + WS_MARKER_FRAME_MAX + 4 + WS_EVENT_FRAME_MAX, "marker + largest event");

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
static uint8_t status_batch[WS_BATCH_SIZE];

// The marker a session gets ahead of its events when it missed n of them: an EventData at
// warning level, stamped now (the times of the lost events are unknown).
static size_t ws_encode_marker(uint8_t* buf, size_t size, uint32_t n)
{
    char text[64];
    snprintf(text, sizeof(text), "status link: %" PRIu32 " event%s not delivered", n, n == 1 ? " was" : "s were");
    return encode_event(buf, size, EV_WARNING, text);
}

// A server WebSocket frame header (FIN, binary, unmasked) for a payload of len < 65536, as
// httpd_ws_send_frame_async writes it. Returns its length: 2 below 126 B, else 4.
static size_t ws_frame_header(uint8_t* h, size_t len)
{
    h[0] = 0x82;
    if (len < 126)
    {
        h[1] = (uint8_t)len;
        return 2;
    }
    h[1] = 126;
    h[2] = (uint8_t)(len >> 8);
    h[3] = (uint8_t)len;
    return 4;
}

// Appends the view's events, after a marker if the session missed some. True if all went in.
static bool ws_append_events(uint8_t* buf, size_t size, size_t* pos, struct ws_batch_plan* plan)
{
    const struct ws_lane_view* lane = &plan->lane;
    if (lane->ev_missed)
    {
        if (size - *pos < 2 + WS_MARKER_FRAME_MAX)
            return false;
        size_t n = ws_encode_marker(buf + *pos + 2, WS_MARKER_FRAME_MAX, lane->ev_missed);
        if (n == 0)
            return false; // logged, cannot happen: nothing past the gap goes out uncounted
        *pos += ws_frame_header(buf + *pos, n) + n;
        plan->marker = true;
    }
    plan->ev_next = lane->ev_from;
    for (uint32_t seq = lane->ev_from; seq != lane->ev_end; ++seq)
    {
        if (size - *pos < 4 + 1)
            return false;
        bool gone;
        size_t n = ws_state_copy_event(seq, buf + *pos + 4, size - *pos - 4, &gone);
        if (n == 0)
            return false; // full, or evicted since the view (the next view counts it as missed)
        if (n < 126)
            memmove(buf + *pos + 2, buf + *pos + 4, n); // copied behind the longest header
        *pos += ws_frame_header(buf + *pos, n) + n;
        plan->ev_next = seq + 1;
        plan->events++;
    }
    return true;
}

// Appends the owed state, sw_status then wifi_status, encoded or copied now. True if all of it
// went in.
static bool ws_append_state(uint8_t* buf, size_t size, size_t* pos, const struct ws_owed* owed,
                            struct ws_batch_plan* plan)
{
    // The space checks come first, so the encoder never logs "stream full".
    if (owed->sw)
    {
        if (size - *pos < 2 + WS_SW_FRAME_MAX)
            return false;
        uint32_t gen = ws_state_sw_gen();                                      // the generation first,
        size_t n = encode_load_switch_status(buf + *pos + 2, WS_SW_FRAME_MAX); // then the outputs
        if (n == 0)
            return false; // cannot happen for 6 B; logged by the encoder
        *pos += ws_frame_header(buf + *pos, n) + n;
        plan->sw = true;
        plan->sw_gen = gen;
    }
    if (owed->wifi)
    {
        if (size - *pos < 2 + WS_WIFI_FRAME_MAX)
            return false;
        uint32_t gen;
        size_t n = ws_state_copy_wifi(buf + *pos + 2, WS_WIFI_FRAME_MAX, &gen); // bytes and generation as one pair
        if (n)
        {
            *pos += ws_frame_header(buf + *pos, n) + n;
            plan->wifi = true;
            plan->wifi_gen = gen;
        }
    }
    return true;
}

// Appends the view's sensor samples, oldest first, as many as fit.
static void ws_append_sensors(uint8_t* buf, size_t size, size_t* pos, struct ws_batch_plan* plan)
{
    for (uint32_t s = plan->lane.sensor_from; s != plan->lane.sensor_end; ++s)
    {
        if (size - *pos < 2 + WS_SENSOR_FRAME_MAX)
            return;
        size_t n = ws_state_copy_sensor(s, buf + *pos + 2, WS_SENSOR_FRAME_MAX);
        if (n == 0)
        {
            plan->sensor_merged++; // overwritten since the view
        }
        else
        {
            *pos += ws_frame_header(buf + *pos, n) + n;
            plan->sensors++;
        }
        plan->sensor_next = s + 1;
    }
}

// Builds the next write for the session on fd in buf: complete WebSocket frames, one
// StatusMessage each, at most size bytes. A new session (owed->snapshot) gets its state first,
// then its events and sensor samples. Otherwise: events (after a marker if some were missed),
// then state, then sensor samples. Each part goes in only after the one before went in whole.
// Returns the length, or 0 when nothing can be written now: the session ended or was replaced
// since the owed read, a producer evicted or overwrote what the view held, or an event was
// stored after the view while the state was read (the state then waits behind it).
static size_t ws_build_batch(int fd, const struct ws_owed* owed, uint8_t* buf, size_t size, struct ws_batch_plan* plan)
{
    *plan = (struct ws_batch_plan){.epoch = owed->epoch};
    plan->lane_ok = ws_state_lane_view(fd, &plan->lane) && plan->lane.epoch == owed->epoch;
    if (!plan->lane_ok)
        return 0; // not even owed state: the fd may hold another session by now
    plan->ev_next = plan->lane.ev_cursor;
    plan->sensor_next = plan->lane.sensor_from;
    plan->sensor_merged = plan->lane.sensor_merged;
    size_t pos = 0;
    if (owed->snapshot)
    {
        if (!ws_append_state(buf, size, &pos, owed, plan) || !ws_append_events(buf, size, &pos, plan))
            return pos;
    }
    else
    {
        if (!ws_append_events(buf, size, &pos, plan))
            return pos;
        size_t state_at = pos;
        bool whole = ws_append_state(buf, size, &pos, owed, plan);
        // An event stored after the view may have been stored before the state was read (the
        // warning task pushes "warning action", then cuts the outputs): it goes first, and the
        // state waits for the next write.
        if ((plan->sw || plan->wifi) && ws_state_ev_head() != plan->lane.ev_end)
        {
            plan->sw = plan->wifi = false;
            return state_at;
        }
        if (!whole)
            return pos;
    }
    ws_append_sensors(buf, size, &pos, plan);
    return pos;
}

// One write: one send_fn call (httpd_txrx.c), so over HTTP one tcp_write and over HTTPS one TLS
// record. After a readiness check it is taken whole or fails. A short or failed write closes
// the session at once: a retry could let httpd's own frames (a pong) land inside the batch.
static esp_err_t ws_send_batch(httpd_handle_t server, int fd, const uint8_t* buf, size_t len)
{
    int n = httpd_socket_send(server, fd, (const char*)buf, len, 0);
    if (n < 0 || (size_t)n != len)
    {
        ws_session_failed(server, fd, status_sender.name, n < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE);
        return ESP_FAIL;
    }
    ws_state_write_done(fd); // the stall is over
    return ESP_OK;
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
    status_batches++;
}

typedef enum
{
    WS_SEND_DONE,      // nothing more is owed
    WS_SEND_NOT_READY, // owed, but nothing could be written now: try again soon
    WS_SEND_FAILED,    // a write failed: the session is blocked and being closed
    WS_SEND_MORE,      // WS_BATCHES_PER_FD writes went out and more is owed
} ws_send_t;

// Writes the session on fd what it is owed, packed into writes of at most WS_BATCH_SIZE, one
// readiness check per write. Called only by the one writer of the current mode, for an fd
// whose ctx is its /ws context (ws_state_ctx), not blocked, and a WebSocket.
static ws_send_t ws_send_owed(httpd_handle_t server, int fd)
{
    for (int b = 0; b < WS_BATCHES_PER_FD; ++b)
    {
        struct ws_owed owed;
        if (!ws_state_owed(fd, &owed))
            return WS_SEND_DONE;
        ws_ready_t ready = ws_write_ready(fd, ws_https, WS_TLS_WRITE_MIN_BLOCK);
        if (ready != WS_READY)
        {
            ws_write_skipped(server, fd, ready, true); // stays owed, cursors untouched
            return WS_SEND_NOT_READY;
        }
        struct ws_batch_plan plan;
        size_t len = ws_build_batch(fd, &owed, status_batch, sizeof(status_batch), &plan);
        if (len == 0)
            return WS_SEND_NOT_READY; // nothing written: the 200 ms re-poll, or the producer's
                                      // request, runs the next pass. Never spins.
        if (ws_send_batch(server, fd, status_batch, len) != ESP_OK)
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

// ---- /uart: the bridge (reader side) and the drain (writer side) -------------------------
// The driver's RX ring is the only buffer. The bridge task reads it only to discard (no viewer,
// or shedding); the writer reads one frame at a time, and only when it can deliver it. Over
// HTTPS the writer is the httpd task (one static work item, normally at most 2 in flight), over
// HTTP a task of its own below httpd.

static httpd_handle_t uart_server;               // set before the bridge or the writer exists
static TaskHandle_t volatile uart_writer_handle; // HTTP only; NULL until created
// HTTPS: drain work items by sequence number. The bridge numbers each item it queues (the
// number is the work argument, a value, not a pointer) and the item publishes the newest
// number started, so queued - started counts the items in flight exactly (see
// uart_kick_in_flight), a stale item never re-opens the kick, and a lost datagram heals as soon
// as a later item starts.
static atomic_uint uart_queued_seq;    // bridge: the number of the last item queued
static atomic_uint uart_started_seq;   // httpd: the newest number started
static atomic_uint uart_report;        // the last drain result + 1 (writer to bridge), 0 = none
static atomic_uint uart_last_frame;    // tick of the last frame read (writer to bridge)
static atomic_uint uart_pending_since; // tick at which the bridge saw the oldest unsent byte
static atomic_bool uart_pending;       // uart_pending_since is valid
static atomic_bool uart_wake_req;      // a /uart handshake asks for a drain
// The bridge found no viewer and discarded: a hold in progress ended then (the drain clears it).
static atomic_bool uart_viewers_lost;
static uint8_t uart_scratch[UART_SCRATCH_SIZE];              // the bridge's only: shed and discard reads
static uint8_t uart_frame[UART_FRAME_HEAD + UART_FRAME_MAX]; // the writer's only
static atomic_uint uart_read_seq; // successful driver reads by either reader (overflow episodes)

// Every byte read out of the driver ring, by either reader.
static void uart_count_read(int n)
{
    __atomic_fetch_add(&uart_received_bytes, (uint32_t)n, __ATOMIC_RELAXED);
    atomic_fetch_add(&uart_read_seq, 1);
}

// An overflow is counted, never answered with uart_flush_input: the ISR already resets the FIFO
// on an overrun (uart.c), and a full ring re-enables RX on the next uart_read_bytes, so a flush
// only threw away what was already safe in the ring. An episode is the first FIFO_OVF or
// BUFFER_FULL after a successful read. The counts are floors: the ISR drops events while the
// event queue is full. Bridge only.
static unsigned uart_ep_reads;         // uart_read_seq at the last overflow event
static bool uart_ep_seen;              // an overflow event was seen since boot
static bool uart_ep_ovf, uart_ep_full; // this episode has counted its overrun / its full ring
static bool uart_ovf_logged;
static TickType_t uart_ovf_log_at; // the last overflow log line

static void uart_note_event(const uart_event_t* ev, TickType_t now)
{
    if (ev->type != UART_FIFO_OVF && ev->type != UART_BUFFER_FULL)
        return; // UART_DATA and the rest: the bridge polls the ring length
    bool ovf = ev->type == UART_FIFO_OVF;
    unsigned reads = atomic_load(&uart_read_seq);
    if (!uart_ep_seen || reads != uart_ep_reads)
    {
        uart_ep_seen = true;
        uart_ep_ovf = uart_ep_full = false;
    }
    uart_ep_reads = reads;
    if (ovf && !uart_ep_ovf)
    {
        uart_ep_ovf = true;
        uart_fifo_overflows++;
    }
    if (!ovf && !uart_ep_full)
    {
        uart_ep_full = true;
        uart_buffer_full_events++;
    }
    // A full ring raises an event about every 0.85 ms: one line per second at most.
    if (!uart_ovf_logged || now - uart_ovf_log_at >= pdMS_TO_TICKS(1000))
    {
        uart_ovf_logged = true;
        uart_ovf_log_at = now;
        ESP_LOGW(TAG, "UART overflow: %lu FIFO overrun and %lu ring-full episodes so far",
                 (unsigned long)uart_fifo_overflows, (unsigned long)uart_buffer_full_events);
    }
}

typedef enum
{
    UART_DRAIN_DONE,      // nothing (more) due
    UART_DRAIN_MORE,      // UART_DRAIN_FRAMES went out and UART_WAKE_BYTES or more still wait
    UART_DRAIN_HELD,      // the bytes stay in the ring: low heap, or viewers not ready
    UART_DRAIN_NO_VIEWER, // nobody to send to: the bridge discards
    UART_DRAIN_BUSY,      // the bridge held rx_mux: again after a tick
} uart_drain_t;

// A frame is due at UART_WAKE_BYTES, once the oldest byte has waited UART_WAKE_AGE_MS, or at
// once if no frame was taken for UART_IDLE_MS (keystroke echo). The bridge wakes the writer by
// this rule, and the writer takes its first frame by it, so a late or duplicate wake never
// sends a sliver.
static bool uart_frame_due(size_t buffered, TickType_t now)
{
    if (buffered == 0)
        return false;
    if (buffered >= UART_WAKE_BYTES)
        return true;
    if (now - (TickType_t)atomic_load(&uart_last_frame) >= pdMS_TO_TICKS(UART_IDLE_MS))
        return true;
    return atomic_load(&uart_pending) &&
           now - (TickType_t)atomic_load(&uart_pending_since) >= pdMS_TO_TICKS(UART_WAKE_AGE_MS);
}

// The writer's per-session /uart state, by lwIP fd slot. Only the one writer of the mode
// touches it, so it needs no lock. An entry belongs to the session whose ws_state epoch it
// holds, and is reset when the fd holds a new session.
struct uart_lag
{
    uint32_t epoch;
    bool lagging;   // was not ready past the hold cap: loses frames instead of holding the others
    uint8_t streak; // lagging: ready frames in a row with the ring below UART_LAG_REGATE_RING
};
static struct uart_lag uart_lags[CONFIG_LWIP_MAX_SOCKETS];

struct uart_viewer
{
    int fd;
    int slot;
    uint32_t epoch;
    ws_ready_t ready;
};

static uint32_t uart_epoch(int fd)
{
    struct ws_owed owed;
    ws_state_owed(fd, &owed); // for the epoch only: a /uart session is never owed anything
    return owed.epoch;
}

// Our context, not blocked, an open WebSocket.
static bool uart_is_viewer(httpd_handle_t server, int fd)
{
    return httpd_sess_get_ctx(server, fd) == &uart_ws_session && !ws_state_blocked(fd) &&
           httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static size_t uart_viewers(httpd_handle_t server, struct uart_viewer* v)
{
    int fds[MAX_CLIENT];
    size_t clients = MAX_CLIENT;
    if (httpd_get_client_list(server, &clients, fds) != ESP_OK)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < clients; ++i)
    {
        int slot;
        if (!uart_is_viewer(server, fds[i]) || !ws_state_slot(fds[i], &slot))
            continue;
        uint32_t epoch = uart_epoch(fds[i]);
        if (uart_lags[slot].epoch != epoch)
            uart_lags[slot] = (struct uart_lag){.epoch = epoch}; // a new session: it gates
        v[n++] = (struct uart_viewer){.fd = fds[i], .slot = slot, .epoch = epoch, .ready = WS_READY};
    }
    return n;
}

// The session v was collected for still holds its fd (HTTP: httpd runs above the writer and
// may have closed it, and a new session may hold the fd by now).
static bool uart_viewer_still(httpd_handle_t server, const struct uart_viewer* v)
{
    return uart_is_viewer(server, v->fd) && uart_epoch(v->fd) == v->epoch;
}

// One viewer did not get n bytes of a frame.
static void uart_viewer_lost(size_t n)
{
    uart_viewer_drop_bytes += n;
    __atomic_fetch_add(&uart_queue_drops, 1, __ATOMIC_RELAXED);
}

// Writer only. A hold for viewers that are not ready lasts from the first such HELD drain until a
// frame is read (a low-heap hold does not start or end it).
static bool uart_holding;
static TickType_t uart_hold_since;

static uart_drain_t uart_hold(TickType_t now)
{
    if (!uart_holding)
    {
        uart_holding = true;
        uart_hold_since = now;
    }
    return UART_DRAIN_HELD;
}

// Sends up to max_frames frames from the driver ring to every /uart viewer: one write per
// viewer per frame (one TLS record over HTTPS). The only /uart writer of each mode: the httpd
// task over HTTPS, uart_writer over HTTP. The ring is read only once the frame can go out, so
// what cannot go out yet waits in the ring, where the bridge sheds it (counted) if it fills.
static uart_drain_t uart_drain(httpd_handle_t server, unsigned max_frames)
{
    // The bridge found no viewer and discarded since the last drain: a hold in progress ended
    // then. Without this a viewer that connects later would inherit the stale hold (no
    // UART_HOLD_MS_MAX grace before it is marked lagging).
    if (atomic_exchange(&uart_viewers_lost, false))
        uart_holding = false;
    for (unsigned f = 0; f < max_frames; ++f)
    {
        TickType_t now = xTaskGetTickCount();
        size_t buffered = 0;
        uart_get_buffered_data_len(UART_NUM, &buffered);
        // The first frame once one is due, the next ones only while a wake's worth waits.
        if (f == 0 ? !uart_frame_due(buffered, now) : buffered < UART_WAKE_BYTES)
        {
            if (buffered == 0)
                uart_holding = false;
            return UART_DRAIN_DONE;
        }
        struct uart_viewer v[MAX_CLIENT];
        size_t n = uart_viewers(server, v);
        if (n == 0)
        {
            uart_holding = false;
            return UART_DRAIN_NO_VIEWER;
        }
        // Global, never a per-viewer skip, and no stall clock: the heap is no viewer's fault, so
        // it does not use up their hold grace either.
        if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < UART_MIN_FREE_HEAP)
            return UART_DRAIN_HELD;

        // Each viewer must take the frame without blocking.
        size_t ready = 0;
        bool gate = false; // a viewer that is not lagging is not ready
        for (size_t i = 0; i < n; ++i)
        {
            v[i].ready = ws_write_ready(v[i].fd, ws_https, WS_TLS_UART_MIN_BLOCK);
            if (v[i].ready == WS_READY)
                ready++;
            else if (!uart_lags[v[i].slot].lagging)
                gate = true;
        }
        bool young = !uart_holding || now - uart_hold_since < pdMS_TO_TICKS(UART_HOLD_MS_MAX);
        if (ready == 0 || (gate && buffered < UART_HOLD_RING_MAX && young))
        {
            // The stall clock of a not-ready viewer runs, so a dead one alone is still closed
            // after WS_STALL_CLOSE_MS while the bridge sheds. Over HTTP only while the fd still
            // holds that session: httpd runs above the writer, and a stall clock run on a new
            // session (one a plain request holds keeps the old stall) could close it.
            for (size_t i = 0; i < n; ++i)
                if (v[i].ready != WS_READY && (ws_https || uart_viewer_still(server, &v[i])))
                    ws_stall_clock(server, v[i].fd, v[i].ready, false);
            return uart_hold(now);
        }

        uint8_t* payload = uart_frame + UART_FRAME_HEAD;
        int got = uart_read_bytes(UART_NUM, payload, buffered < UART_FRAME_MAX ? buffered : UART_FRAME_MAX, 0);
        if (got < 0)
            return UART_DRAIN_BUSY; // rx_mux: the bridge is reading. Busy, not empty.
        uart_holding = false;
        if (got == 0)
            return UART_DRAIN_DONE;
        // Past the cap a not-ready viewer stops holding the ready ones: it is marked lagging,
        // and loses this frame. Only once a frame was read: a BUSY or empty run sends nothing,
        // so it changes no viewer's gating.
        for (size_t i = 0; i < n; ++i)
        {
            struct uart_lag* lag = &uart_lags[v[i].slot];
            if (v[i].ready != WS_READY && !lag->lagging)
            {
                lag->lagging = true;
                lag->streak = 0;
                uart_lagging_marks++;
            }
        }
        uart_count_read(got);
        uart_frame_bytes += (uint32_t)got; // right after the read count, as uart_discard does
        atomic_store(&uart_last_frame, now);

        uint8_t h[UART_FRAME_HEAD];
        size_t head = ws_frame_header(h, got); // right before the payload: one write
        memcpy(payload - head, h, head);
        const char* buf = (const char*)payload - head;
        size_t len = head + got;
        static uint32_t rotate;
        size_t first = rotate++ % n; // a different viewer goes first every frame
        for (size_t k = 0; k < n; ++k)
        {
            struct uart_viewer* p = &v[(first + k) % n];
            struct uart_lag* lag = &uart_lags[p->slot];
            if (!ws_https && !uart_viewer_still(server, p))
            {
                uart_viewer_lost(got); // closed or replaced meanwhile: never written, no stall clock
                continue;
            }
            if (p->ready != WS_READY)
            {
                lag->streak = 0;
                uart_viewer_lost(got);
                ws_write_skipped(server, p->fd, p->ready, false); // counted; the stall clock runs
                continue;
            }
            int w = httpd_socket_send(server, p->fd, buf, len, 0);
            if (w < 0 || (size_t)w != len)
            {
                // A short or failed write closes the session, with no retry: a retry could put
                // httpd's own frames (a pong) inside this one. Never a session that took the fd
                // over during the write.
                uart_viewer_lost(got);
                if (ws_https || uart_viewer_still(server, p))
                    ws_session_failed(server, p->fd, "ws-uart", w < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE);
                continue;
            }
            ws_state_write_done(p->fd); // the stall is over
            if (lag->lagging)
            {
                if (buffered >= UART_LAG_REGATE_RING)
                    lag->streak = 0;
                else if (++lag->streak >= UART_LAG_REGATE_FRAMES)
                {
                    lag->lagging = false;
                    lag->streak = 0;
                }
            }
        }
    }
    size_t left = 0;
    uart_get_buffered_data_len(UART_NUM, &left);
    return left >= UART_WAKE_BYTES ? UART_DRAIN_MORE : UART_DRAIN_DONE;
}

// HTTPS: the drain, in the httpd task. Static, and its argument is only its sequence number
// (no allocation), so a duplicate only drains what is there.
static void uart_drain_work(void* arg)
{
    unsigned seq = (unsigned)(uintptr_t)arg;
    // First: from now on a wake needs a new item. Only httpd writes started, so a plain max();
    // an older (stale) item leaves it as it is.
    if ((int32_t)(seq - atomic_load(&uart_started_seq)) > 0)
        atomic_store(&uart_started_seq, seq);
    atomic_store(&uart_report, (unsigned)uart_drain(uart_server, UART_DRAIN_FRAMES) + 1);
}

// HTTPS: drain items queued and not started yet. The bridge stores an item's number before it
// queues it (httpd may start it before httpd_queue_work returns) and takes it back if the queue
// failed, so started never passes queued; a diagnostics read of the two can still see that for
// an instant, hence the clamp.
static unsigned uart_kick_in_flight(void)
{
    int32_t d = (int32_t)(atomic_load(&uart_queued_seq) - atomic_load(&uart_started_seq));
    return d > 0 ? (unsigned)d : 0;
}

// HTTP: wakes the writer. NULL-safe: xTaskNotifyGive(NULL) trips configASSERT.
static void uart_wake_writer(void)
{
    TaskHandle_t task = uart_writer_handle;
    if (task)
        xTaskNotifyGive(task);
}

// HTTP: the /uart writer, below httpd.
static void uart_writer(void* arg)
{
    httpd_handle_t server = arg;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UART_WRITER_IDLE_MS));
        uart_drain_t r;
        do
            r = uart_drain(server, UART_DRAIN_FRAMES);
        while (r == UART_DRAIN_MORE); // each further frame is >= UART_WAKE_BYTES: paced by the UART
        atomic_store(&uart_report, (unsigned)r + 1);
        // Never spins: a held drain waits UART_HOLD_RETRY_MS, a busy one a tick. Wakes that
        // arrive meanwhile are kept by the notification count.
        if (r == UART_DRAIN_HELD || r == UART_DRAIN_NO_VIEWER)
            vTaskDelay(pdMS_TO_TICKS(UART_HOLD_RETRY_MS));
        else if (r == UART_DRAIN_BUSY)
            vTaskDelay(1);
    }
}

// Reads and discards up to len of the oldest buffered bytes, each chunk counted in bucket as it
// is read. Returns how many.
static size_t uart_discard(size_t len, volatile uint32_t* bucket)
{
    size_t done = 0;
    while (done < len)
    {
        size_t want = len - done < sizeof(uart_scratch) ? len - done : sizeof(uart_scratch);
        int got = uart_read_bytes(UART_NUM, uart_scratch, want, 0);
        if (got <= 0)
            break; // -1: the writer is reading (next pass); 0: empty
        uart_count_read(got);
        *bucket += (uint32_t)got;
        done += (size_t)got;
    }
    return done;
}

// The reader side: it never writes a socket. It watches the ring, counts overflow episodes,
// discards when there is no viewer, sheds when the writer is stuck, and wakes the writer when a
// frame is due (HTTP: a notification; HTTPS: a kick of the drain work item into httpd).
static void uart_bridge(void* arg)
{
    httpd_handle_t server = arg;
    bool wait = false; // no wake before wait_at: the last drain held, or a kick failed
    TickType_t wait_at = 0;
    TickType_t kicked_at = 0;     // HTTPS: when the last drain item was queued
    unsigned seen_last_frame = 0; // uart_last_frame when uart_pending_since was set
    for (;;)
    {
        size_t buffered = 0;
        uart_event_t ev;
        uart_get_buffered_data_len(UART_NUM, &buffered);
        if (buffered == 0)
        {
            // Empty: sleep until the driver posts an event (UART_DATA after 120 B or 10 idle
            // symbols), with a timeout in case that event was dropped.
            if (xQueueReceive(uart_event_queue, &ev, pdMS_TO_TICKS(UART_BRIDGE_IDLE_MS)) == pdPASS)
                uart_note_event(&ev, xTaskGetTickCount());
        }
        else
        {
            vTaskDelay(1); // data waits: one pass per tick, never a spin
        }
        TickType_t now = xTaskGetTickCount();
        while (xQueueReceive(uart_event_queue, &ev, 0) == pdPASS)
            uart_note_event(&ev, now);

        uart_get_buffered_data_len(UART_NUM, &buffered);
        unsigned report = atomic_exchange(&uart_report, 0);
        if (report == UART_DRAIN_HELD + 1 || report == UART_DRAIN_NO_VIEWER + 1)
        {
            wait = true;
            wait_at = now + pdMS_TO_TICKS(UART_HOLD_RETRY_MS);
        }
        else if (wait && due(now, wait_at))
            wait = false; // never left set: due() is wrap-safe only within 2^31 ticks

        if (buffered == 0)
        {
            atomic_store(&uart_pending, false);
            continue;
        }
        if (uart_websocket_client_connected(server) == UART_SEEN_NONE)
        {
            // No viewer: read and discard (no history).
            if (uart_discard(buffered, &uart_no_viewer_bytes))
            {
                atomic_store(&uart_viewers_lost, true); // a hold in progress ended: the next drain clears it
            }
            atomic_store(&uart_pending, false);
            continue;
        }
        unsigned last_frame = atomic_load(&uart_last_frame);
        if (!atomic_load(&uart_pending) || last_frame != seen_last_frame)
        {
            // Approximate after a frame: the bytes left behind it are taken as new.
            atomic_store(&uart_pending_since, now);
            atomic_store(&uart_pending, true);
            seen_last_frame = last_frame;
        }
        // No frame for UART_SHED_STALL_MS and the ring nearly full: shed the oldest bytes,
        // counted, before RX interrupts go off and the FIFO overruns uncounted.
        if (buffered >= UART_SHED_HIGH &&
            now - (TickType_t)atomic_load(&uart_last_frame) >= pdMS_TO_TICKS(UART_SHED_STALL_MS))
        {
            size_t n = uart_discard(buffered - UART_SHED_LOW, &uart_shed_bytes);
            if (n)
            {
                __atomic_fetch_add(&uart_queue_drops, 1, __ATOMIC_RELAXED);
                buffered -= n;
            }
        }

        if (wait && !due(now, wait_at))
            continue; // a held drain is retried after UART_HOLD_RETRY_MS, not every tick
        if (!atomic_exchange(&uart_wake_req, false) && !uart_frame_due(buffered, now))
            continue;
        wait = false;
        if (!ws_https)
        {
            uart_wake_writer();
            continue;
        }
        // At most one item in flight, two once one has waited UART_KICK_REARM_MS (its datagram
        // was lost, or httpd is stuck); past that one more per UART_KICK_FALLBACK_MS, in case
        // every datagram in flight was lost. A duplicate only drains what is there.
        unsigned in_flight = uart_kick_in_flight();
        TickType_t age = now - kicked_at;
        bool rearm = in_flight == 1 && age >= pdMS_TO_TICKS(UART_KICK_REARM_MS);
        bool fallback = in_flight >= 2 && age >= pdMS_TO_TICKS(UART_KICK_FALLBACK_MS);
        if (in_flight && !rearm && !fallback)
            continue; // queued and not started: it reads the ring when it runs
        unsigned seq = atomic_load(&uart_queued_seq) + 1;
        atomic_store(&uart_queued_seq, seq); // before the queue: httpd may start the item first
        if (httpd_queue_work(server, uart_drain_work, (void*)(uintptr_t)seq) == ESP_OK)
        {
            kicked_at = now;
        }
        else
        {
            atomic_store(&uart_queued_seq, seq - 1); // not queued: nobody can start it
            uart_kick_failures++;
            wait = true;
            wait_at = now + pdMS_TO_TICKS(UART_KICK_RETRY_MS);
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

// /uart, in the httpd task right after ws_handshake_done is set: the next drain sees the new
// viewer. It only wakes; httpd fails the request unless this returns ESP_OK.
static esp_err_t uart_post_handshake_cb(httpd_req_t* req)
{
    (void)req;
    atomic_store(&uart_wake_req, true); // the bridge wakes the writer (HTTPS: kicks) on its next pass
    if (!ws_https)
        uart_wake_writer();
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
        .ws_post_handshake_cb = uart_post_handshake_cb,
    };
    httpd_register_uri_handler(server, &ws);
    httpd_register_uri_handler(server, &uart_ws);

    // No status queue: events and sensor samples are held in ws_state.c's lanes.
    status_sender = (struct ws_sender_config){.server = server, .name = "ws-status"};
    // The stack size leaves 1.3-2.3 KB above the deepest use measured while page loads, both
    // streams and an SBC boot log were running and send failures were logged (~1.7 KB).
    TaskHandle_t task = NULL;
    xTaskCreate(ws_status_sender_task, "ws_status_sender", 1024 * 3, &status_sender, 9, &task);
    status_sender_task = task;

    // /uart: the server handle and the writer exist before the bridge, which preempts this
    // task as soon as it is created (priority 12 over HTTPS) and wakes the writer.
    uart_server = server;
    if (!ws_https)
    {
        TaskHandle_t writer = NULL;
        if (xTaskCreate(uart_writer, "uart_writer", UART_WRITER_STACK, server, UART_WRITER_PRIO, &writer) != pdPASS)
            ESP_LOGE(TAG, "uart_writer not created: /uart sends nothing");
        uart_writer_handle = writer;
    }
    if (xTaskCreate(uart_bridge, "uart_bridge", UART_BRIDGE_STACK, server,
                    ws_https ? UART_BRIDGE_PRIO_HTTPS : UART_BRIDGE_PRIO_HTTP, NULL) != pdPASS)
        ESP_LOGE(TAG, "uart_bridge not created: the UART ring is not read");
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
    if (uart_event_queue)
    {
        uart_get_buffered_data_len(UART_NUM, &diagnostics->uart_buffered_bytes);
        // The /uart queue is the driver ring: bytes waiting, and its size.
        diagnostics->queue_depth = diagnostics->uart_buffered_bytes;
        diagnostics->queue_capacity = UART_RX_BUFFER_SIZE;
    }

    diagnostics->uart_received_bytes = uart_received_bytes;
    diagnostics->uart_fifo_overflows = uart_fifo_overflows;
    diagnostics->uart_buffer_full_events = uart_buffer_full_events;
    diagnostics->uart_queue_drops = uart_queue_drops;
    diagnostics->uart_frame_bytes = uart_frame_bytes;
    diagnostics->uart_shed_bytes = uart_shed_bytes;
    diagnostics->uart_no_viewer_bytes = uart_no_viewer_bytes;
    diagnostics->uart_viewer_drop_bytes = uart_viewer_drop_bytes;
    diagnostics->uart_lagging_marks = uart_lagging_marks;
    diagnostics->uart_kick_failures = uart_kick_failures;
    diagnostics->websocket_send_failures = websocket_send_failures;
    diagnostics->websocket_close_failures = websocket_close_failures;
    diagnostics->websocket_close_retries = websocket_close_retries;
    diagnostics->websocket_not_ready_skips = websocket_not_ready_skips;
    diagnostics->websocket_heap_skips = websocket_heap_skips;
    diagnostics->websocket_stall_closes = websocket_stall_closes;
    // The status queue and its drop points are gone (events and sensor samples are held in
    // ws_state.c's lanes until sent): status_drops_queue_full, _https_pending, _ctrl_queue,
    // _no_memory, status_sw_drops and status_https_pending stay 0. status_queue_drops now
    // reports the events sessions missed, as the page and pmtui show it as status drops.
    struct ws_lane_stats lanes;
    ws_state_lane_stats(&lanes);
    diagnostics->status_queue_drops = lanes.events_missed;
    diagnostics->status_drops_no_client = lanes.ev_no_session;
    diagnostics->status_events_stored = lanes.ev_stored;
    diagnostics->status_events_missed = lanes.events_missed;
    diagnostics->status_events_lost_on_close = lanes.events_lost_on_close;
    diagnostics->status_sensor_merged = lanes.sensor_merged;
    diagnostics->status_event_backlog_max = lanes.backlog_max;
    diagnostics->status_event_backlog_max_bytes = lanes.backlog_max_bytes;
    diagnostics->status_batches = status_batches;
    diagnostics->status_stall_closes = status_stall_closes;
    diagnostics->uart_https_pending = uart_kick_in_flight(); // HTTPS: drain items queued, not started
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
