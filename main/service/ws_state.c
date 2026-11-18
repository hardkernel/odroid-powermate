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
// is a WebSocket, so a stale entry is never read for another session.
struct ws_fd_state
{
    uint32_t epoch;     // ++ at each WebSocket handshake on this fd
    uint32_t sw_sent;   // sw_gen whose outputs were last written to this session, 0 = none
    uint32_t wifi_sent; // wifi_gen last written, 0 = none
    bool blocked;       // a write failed: its close was triggered, write nothing more
};

static struct ws_fd_state fds[WS_FD_SLOTS];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED; // guards fds[] and the Wi-Fi slot

// All atomics are sequentially consistent: the work item's kick = 0 must stay ahead of its
// generation loads (an acquire load could move before an earlier store).
static atomic_uint sw_gen;   // 0 = outputs not read from the hardware yet
static atomic_uint wifi_gen; // 0 = no Wi-Fi status yet; written only under lock
static uint8_t wifi_frame[WS_WIFI_FRAME_MAX];
static size_t wifi_len; // under lock
static atomic_bool pass_needed;  // a state pass must run (producer to sender)
static atomic_int kick;          // HTTPS: 1 while a state work item is queued and not started
static atomic_uint pass_report;  // HTTPS: 0 = no news, else re-poll ms + 1 (httpd to sender)

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

void ws_state_session_opened(int fd)
{
    int i;
    if (!ws_state_slot(fd, &i))
        return;
    portENTER_CRITICAL(&lock);
    fds[i].epoch++;
    fds[i].sw_sent = 0;
    fds[i].wifi_sent = 0;
    fds[i].blocked = false;
    portEXIT_CRITICAL(&lock);
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

bool ws_state_owed(int fd, struct ws_owed* o)
{
    int i;
    o->epoch = 0;
    o->sw = o->wifi = false;
    if (!ws_state_slot(fd, &i))
        return false;
    portENTER_CRITICAL(&lock);
    uint32_t epoch = fds[i].epoch;
    uint32_t sw_sent = fds[i].sw_sent;
    uint32_t wifi_sent = fds[i].wifi_sent;
    portEXIT_CRITICAL(&lock);
    uint32_t g_sw = atomic_load(&sw_gen);
    uint32_t g_wifi = atomic_load(&wifi_gen);
    o->epoch = epoch;
    o->sw = g_sw != 0 && sw_sent != g_sw;
    o->wifi = g_wifi != 0 && wifi_sent != g_wifi;
    return o->sw || o->wifi;
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
