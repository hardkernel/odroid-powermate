/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "ws_state.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h" // LWIP_SOCKET_OFFSET
#include "sdkconfig.h"

// One entry per lwIP socket: fds are LWIP_SOCKET_OFFSET .. LWIP_SOCKET_OFFSET + MAX_SOCKETS - 1.
#define WS_FD_SLOTS CONFIG_LWIP_MAX_SOCKETS

// Nothing prunes an entry when its session closes. Every per-session field is reset at the
// fd's next WebSocket handshake, and every reader also checks the session's ctx and that it
// is a WebSocket, so a stale entry is never read for another session. A /ws session is also
// marked closed by its free_ctx, after which it is owed nothing.
struct ws_fd_state
{
    uint32_t epoch;       // ++ at each WebSocket handshake on this fd and at a /ws session's free_ctx
    uint32_t sw_sent;     // sw_gen whose outputs were last written to this session, 0 = none
    uint32_t wifi_sent;   // wifi_gen last written, 0 = none
    uint32_t ev_next;     // seq of the next event this /ws session is to get (while open)
    uint32_t sensor_next; // seq of the next sensor sample it is to get (while open)
    bool blocked;         // a write failed: its close was triggered, write nothing more
    bool close_retry;     // that close could not be queued: a state pass retries it
    bool stalled;         // writes are being skipped (not ready) since stall_since
    bool stall_heap;      // the last skip was for the TLS heap only
    bool open;            // a /ws session between its handshake and its free_ctx
    TickType_t stall_since;
    TickType_t last_skip; // when the last write was skipped
};

static struct ws_fd_state fds[WS_FD_SLOTS];
// Guards fds[], the Wi-Fi slot and the lanes. On the C3 it masks interrupts, so every section
// stays a few microseconds: at most one record is copied per section (the UART RX FIFO
// overflows after ~53 us at 1.5 Mbaud).
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

// All atomics are sequentially consistent: the work item's kick = 0 must stay ahead of its
// generation loads (an acquire load could move before an earlier store).
static atomic_uint sw_gen;   // 0 = outputs not read from the hardware yet
static atomic_uint wifi_gen; // 0 = no Wi-Fi status yet; written only under lock
static uint8_t wifi_frame[WS_WIFI_FRAME_MAX];
static size_t wifi_len; // under lock
static atomic_bool pass_needed;  // a state pass must run (producer to sender)
static atomic_int kick;          // HTTPS: 1 while a state work item is queued and not started
static atomic_uint pass_report;  // HTTPS: 0 = no news, else re-poll ms + 1 (httpd to sender)

// ---- Lanes (all under lock) ---------------------------------------------------------------
_Static_assert((WS_EV_SLOTS & (WS_EV_SLOTS - 1)) == 0 && WS_EV_SLOTS <= 255, "WS_EV_SLOTS: a power of two");
_Static_assert(WS_EVENT_FRAME_MAX <= WS_EV_RING_SIZE && WS_EV_RING_SIZE <= UINT16_MAX, "event ring size");
_Static_assert(WS_SENSOR_FRAME_MAX <= 255, "a sensor frame length must fit a uint8_t");

// Event seq s is held while ev_tail <= s < ev_head, in ev_idx[s % WS_EV_SLOTS]. Each record is
// contiguous: the live bytes are [start, ev_wr) ("straight", ev_wr > start) or
// [start, end of the last record before the wrap) + [0, ev_wr) ("wrapped", ev_wr <= start),
// where start is the oldest record's offset.
struct ws_ev_ref
{
    uint16_t off;
    uint16_t len;
};
static uint8_t ev_ring[WS_EV_RING_SIZE];
static struct ws_ev_ref ev_idx[WS_EV_SLOTS];
static uint32_t ev_head; // seq the next event gets = events stored since boot
static uint32_t ev_tail; // oldest seq held; 0 <= ev_head - ev_tail <= WS_EV_SLOTS
static uint16_t ev_wr;   // offset just after the newest record

// Sample s is held while 1 <= sensor_seq - s <= WS_SENSOR_SLOTS (and its slot is not empty), in
// slot (sensor_wr + WS_SENSOR_SLOTS - (sensor_seq - s)) % WS_SENSOR_SLOTS.
struct ws_sensor_slot
{
    uint8_t len;
    uint8_t frame[WS_SENSOR_FRAME_MAX];
};
static struct ws_sensor_slot sensor[WS_SENSOR_SLOTS];
static uint32_t sensor_seq; // samples stored since boot (seq of the next one)
static uint8_t sensor_wr;   // slot the next sample goes to

static uint32_t open_sessions; // /ws entries with open set
static uint32_t ev_no_session; // events stored while open_sessions was 0

// 0 means "nothing known yet", so a wrap skips it.
static uint32_t bump(atomic_uint* gen)
{
    uint32_t g = atomic_fetch_add(gen, 1) + 1;
    if (g == 0)
        g = atomic_fetch_add(gen, 1) + 1;
    return g;
}

uint32_t ws_state_bump_sw(void)
{
    return bump(&sw_gen);
}

bool ws_state_store_wifi(const uint8_t* frame, size_t len)
{
    if (len == 0 || len > WS_WIFI_FRAME_MAX)
        return false;
    portENTER_CRITICAL(&lock);
    memcpy(wifi_frame, frame, len);
    wifi_len = len;
    bump(&wifi_gen);
    portEXIT_CRITICAL(&lock);
    return true;
}

uint32_t ws_state_sw_gen(void)
{
    return atomic_load(&sw_gen);
}

uint32_t ws_state_wifi_gen(void)
{
    return atomic_load(&wifi_gen);
}

size_t ws_state_copy_wifi(uint8_t* buf, size_t size, uint32_t* gen)
{
    size_t len = 0;
    portENTER_CRITICAL(&lock);
    uint32_t g = atomic_load(&wifi_gen);
    if (g != 0 && wifi_len <= size)
    {
        memcpy(buf, wifi_frame, wifi_len);
        len = wifi_len;
    }
    portEXIT_CRITICAL(&lock);
    *gen = g;
    return len;
}

bool ws_state_slot(int fd, int* slot)
{
    int i = fd - LWIP_SOCKET_OFFSET;
    if (i < 0 || i >= WS_FD_SLOTS)
        return false;
    if (slot)
        *slot = i;
    return true;
}

void* ws_state_ctx(int fd)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return NULL;
    return &fds[i];
}

bool ws_state_ctx_is(int fd, const void* ctx)
{
    return ctx != NULL && ctx == ws_state_ctx(fd);
}

// Under lock. A /ws session on slot i ends.
static void session_closed(int i)
{
    fds[i].open = false;
    open_sessions--;
}

void ws_state_session_opened(int fd, bool status)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return;
    portENTER_CRITICAL(&lock);
    if (fds[i].open)
        session_closed(i); // its free_ctx never ran: httpd frees a session's ctx before it accepts again
    fds[i].epoch++;
    fds[i].sw_sent = 0;
    fds[i].wifi_sent = 0;
    fds[i].blocked = false;
    fds[i].close_retry = false;
    fds[i].stalled = false;
    fds[i].stall_heap = false;
    fds[i].open = status;
    if (status)
    {
        // No replay: the session gets what is stored from now on.
        fds[i].ev_next = ev_head;
        fds[i].sensor_next = sensor_seq;
        open_sessions++;
    }
    portEXIT_CRITICAL(&lock);
}

void ws_state_ctx_free(void* ctx)
{
    for (int i = 0; i < WS_FD_SLOTS; ++i)
    {
        if (ctx != &fds[i])
            continue;
        portENTER_CRITICAL(&lock);
        if (fds[i].open)
        {
            session_closed(i);
            fds[i].epoch++; // a batch built for it commits and marks nothing
        }
        portEXIT_CRITICAL(&lock);
        return;
    }
}

static void set_blocked(int fd, bool blocked)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return;
    portENTER_CRITICAL(&lock);
    fds[i].blocked = blocked;
    portEXIT_CRITICAL(&lock);
}

void ws_state_block(int fd)
{
    set_blocked(fd, true);
}

void ws_state_unblock(int fd)
{
    set_blocked(fd, false);
}

bool ws_state_blocked(int fd)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return true;
    portENTER_CRITICAL(&lock);
    bool blocked = fds[i].blocked;
    portEXIT_CRITICAL(&lock);
    return blocked;
}

void ws_state_close_failed(int fd)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return;
    portENTER_CRITICAL(&lock);
    fds[i].close_retry = true;
    portEXIT_CRITICAL(&lock);
}

bool ws_state_take_close_retry(int fd)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return false;
    portENTER_CRITICAL(&lock);
    bool retry = fds[i].close_retry;
    fds[i].close_retry = false;
    portEXIT_CRITICAL(&lock);
    return retry;
}

void ws_state_write_skipped(int fd, TickType_t now, bool heap)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return;
    portENTER_CRITICAL(&lock);
    // A heap skip after a gap in the skips starts a new stall. The shortage is global, so it says
    // nothing about the gap, and the socket was writable (writability is checked first). A
    // socket that is still not writable after a gap was not written, so it did not drain: that
    // stall goes on.
    if (!fds[i].stalled || (heap && now - fds[i].last_skip >= pdMS_TO_TICKS(WS_HEAP_STALL_GAP_MS)))
    {
        fds[i].stalled = true;
        fds[i].stall_since = now;
    }
    fds[i].last_skip = now;
    fds[i].stall_heap = heap;
    portEXIT_CRITICAL(&lock);
}

void ws_state_write_done(int fd)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return;
    portENTER_CRITICAL(&lock);
    fds[i].stalled = false;
    portEXIT_CRITICAL(&lock);
}

bool ws_state_stalled_for(int fd, TickType_t now, TickType_t ticks)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return false;
    portENTER_CRITICAL(&lock);
    bool stalled = fds[i].stalled && now - fds[i].stall_since >= ticks;
    portEXIT_CRITICAL(&lock);
    return stalled;
}

bool ws_state_stall_expired(int fd, TickType_t now, bool* heap)
{
    int i;
    *heap = false;
    if (!ws_state_slot(fd, &i))
        return false;
    portENTER_CRITICAL(&lock);
    // Unsigned difference: wrap-safe, a stall is closed long before the tick count wraps.
    bool expired = fds[i].stalled && now - fds[i].stall_since >= pdMS_TO_TICKS(WS_STALL_CLOSE_MS);
    *heap = fds[i].stall_heap;
    portEXIT_CRITICAL(&lock);
    return expired;
}

bool ws_state_owed(int fd, struct ws_owed* o)
{
    int i;
    *o = (struct ws_owed){0};
    if (!ws_state_slot(fd, &i))
        return false;
    portENTER_CRITICAL(&lock);
    uint32_t epoch = fds[i].epoch;
    uint32_t sw_sent = fds[i].sw_sent;
    uint32_t wifi_sent = fds[i].wifi_sent;
    bool open = fds[i].open;
    bool events = open && fds[i].ev_next != ev_head;
    bool samples = open && fds[i].sensor_next != sensor_seq;
    portEXIT_CRITICAL(&lock);
    uint32_t g_sw = atomic_load(&sw_gen);
    uint32_t g_wifi = atomic_load(&wifi_gen);
    o->epoch = epoch;
    o->sw = open && g_sw != 0 && sw_sent != g_sw;
    o->wifi = open && g_wifi != 0 && wifi_sent != g_wifi;
    o->snapshot = o->sw && sw_sent == 0;
    o->events = events;
    o->sensor = samples;
    return o->sw || o->wifi || o->events || o->sensor;
}

static void mark(int fd, uint32_t epoch, uint32_t gen, bool wifi)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return;
    portENTER_CRITICAL(&lock);
    if (fds[i].epoch == epoch)
    {
        if (wifi)
            fds[i].wifi_sent = gen;
        else
            fds[i].sw_sent = gen;
    }
    portEXIT_CRITICAL(&lock);
}

void ws_state_mark_sw(int fd, uint32_t epoch, uint32_t gen)
{
    mark(fd, epoch, gen, false);
}

void ws_state_mark_wifi(int fd, uint32_t epoch, uint32_t gen)
{
    mark(fd, epoch, gen, true);
}

// ---- Lanes -----------------------------------------------------------------------------------

// Under lock. True with *off where a record of len fits without evicting.
static bool ev_fits(uint16_t len, uint16_t* off)
{
    if (ev_head == ev_tail)
    {
        *off = 0;
        return true;
    }
    uint16_t start = ev_idx[ev_tail % WS_EV_SLOTS].off;
    if (ev_wr > start) // straight: free are [ev_wr, end) and [0, start)
    {
        if (WS_EV_RING_SIZE - ev_wr >= len)
        {
            *off = ev_wr;
            return true;
        }
        if (start >= len)
        {
            *off = 0; // [ev_wr, end) stays unused until the tail passes it
            return true;
        }
        return false;
    }
    if (start - ev_wr >= len) // wrapped: free is [ev_wr, start)
    {
        *off = ev_wr;
        return true;
    }
    return false;
}

enum ws_push ws_state_push_event(const uint8_t* frame, size_t len)
{
    if (len == 0 || len > WS_EVENT_FRAME_MAX)
        return WS_PUSH_REFUSED;
    uint16_t off = 0;
    portENTER_CRITICAL(&lock);
    // Evict the oldest until the record fits: at most WS_EV_SLOTS times, an empty ring fits it.
    while (ev_head - ev_tail == WS_EV_SLOTS || !ev_fits((uint16_t)len, &off))
        ev_tail++;
    memcpy(ev_ring + off, frame, len);
    ev_idx[ev_head % WS_EV_SLOTS] = (struct ws_ev_ref){.off = off, .len = (uint16_t)len};
    ev_head++;
    ev_wr = (uint16_t)(off + len);
    bool none = open_sessions == 0;
    if (none)
        ev_no_session++;
    portEXIT_CRITICAL(&lock);
    return none ? WS_PUSH_NO_SESSION : WS_PUSH_STORED;
}

enum ws_push ws_state_push_sensor(const uint8_t* frame, size_t len)
{
    if (len == 0 || len > WS_SENSOR_FRAME_MAX)
        return WS_PUSH_REFUSED;
    portENTER_CRITICAL(&lock);
    sensor[sensor_wr].len = (uint8_t)len;
    memcpy(sensor[sensor_wr].frame, frame, len);
    sensor_wr = (uint8_t)((sensor_wr + 1) % WS_SENSOR_SLOTS);
    sensor_seq++;
    bool none = open_sessions == 0;
    portEXIT_CRITICAL(&lock);
    return none ? WS_PUSH_NO_SESSION : WS_PUSH_STORED;
}

uint32_t ws_state_ev_head(void)
{
    portENTER_CRITICAL(&lock);
    uint32_t head = ev_head;
    portEXIT_CRITICAL(&lock);
    return head;
}

bool ws_state_lane_view(int fd, struct ws_lane_view* v)
{
    int i;
    *v = (struct ws_lane_view){0};
    if (!ws_state_slot(fd, &i))
        return false;
    portENTER_CRITICAL(&lock);
    bool open = fds[i].open;
    uint32_t sensor_cursor = 0;
    if (open)
    {
        v->epoch = fds[i].epoch;
        v->ev_cursor = fds[i].ev_next;
        v->ev_end = ev_head;
        // Unsigned distances back from the head: wrap-safe.
        v->ev_from = ev_head - v->ev_cursor > ev_head - ev_tail ? ev_tail : v->ev_cursor;
        sensor_cursor = fds[i].sensor_next;
        v->sensor_end = sensor_seq;
        v->sensor_from =
            sensor_seq - sensor_cursor > WS_SENSOR_SLOTS ? sensor_seq - WS_SENSOR_SLOTS : sensor_cursor;
    }
    portEXIT_CRITICAL(&lock);
    v->ev_missed = v->ev_from - v->ev_cursor;
    v->sensor_merged = v->sensor_from - sensor_cursor;
    return open;
}

size_t ws_state_copy_event(uint32_t seq, uint8_t* buf, size_t size, bool* gone)
{
    size_t n = 0;
    portENTER_CRITICAL(&lock);
    bool held = seq - ev_tail < ev_head - ev_tail;
    bool evicted = (int32_t)(seq - ev_tail) < 0;
    if (held)
    {
        struct ws_ev_ref r = ev_idx[seq % WS_EV_SLOTS];
        if (r.len <= size)
        {
            memcpy(buf, ev_ring + r.off, r.len);
            n = r.len;
        }
    }
    portEXIT_CRITICAL(&lock);
    *gone = evicted;
    return n;
}

size_t ws_state_copy_sensor(uint32_t seq, uint8_t* buf, size_t size)
{
    size_t n = 0;
    portENTER_CRITICAL(&lock);
    uint32_t d = sensor_seq - seq; // 1 for the newest
    if (d >= 1 && d <= WS_SENSOR_SLOTS)
    {
        const struct ws_sensor_slot* slot = &sensor[(sensor_wr + WS_SENSOR_SLOTS - d) % WS_SENSOR_SLOTS];
        if (slot->len != 0 && slot->len <= size)
        {
            memcpy(buf, slot->frame, slot->len);
            n = slot->len;
        }
    }
    portEXIT_CRITICAL(&lock);
    return n;
}

bool ws_state_lane_commit(int fd, uint32_t epoch, uint32_t ev_next, uint32_t ev_missed, uint32_t sensor_next,
                          uint32_t sensor_merged)
{
    int i;
    (void)ev_missed;
    (void)sensor_merged;
    if (!ws_state_slot(fd, &i))
        return false;
    portENTER_CRITICAL(&lock);
    bool ok = fds[i].open && fds[i].epoch == epoch;
    if (ok)
    {
        fds[i].ev_next = ev_next;
        fds[i].sensor_next = sensor_next;
    }
    portEXIT_CRITICAL(&lock);
    return ok;
}

void ws_state_lane_stats(struct ws_lane_stats* s)
{
    portENTER_CRITICAL(&lock);
    s->ev_no_session = ev_no_session;
    portEXIT_CRITICAL(&lock);
}

void ws_state_request_pass(void)
{
    atomic_store(&pass_needed, true);
}

bool ws_state_take_pass_request(void)
{
    return atomic_exchange(&pass_needed, false);
}

enum ws_kick ws_state_kick(bool (*queue)(void* ctx), void* ctx)
{
    int idle = 0;
    if (!atomic_compare_exchange_strong(&kick, &idle, 1))
        return WS_KICK_OUTSTANDING;
    if (queue(ctx))
        return WS_KICK_QUEUED;
    atomic_store(&kick, 0);
    return WS_KICK_FAILED;
}

bool ws_state_kick_outstanding(void)
{
    return atomic_load(&kick) != 0;
}

void ws_state_kick_reset(void)
{
    atomic_store(&kick, 0);
}

void ws_state_pass_started(void)
{
    atomic_store(&kick, 0);
}

void ws_state_report_pass(uint32_t ms)
{
    atomic_store(&pass_report, ms + 1);
}

bool ws_state_take_report(uint32_t* ms)
{
    uint32_t r = atomic_exchange(&pass_report, 0);
    if (r == 0)
        return false;
    *ms = r - 1;
    return true;
}
