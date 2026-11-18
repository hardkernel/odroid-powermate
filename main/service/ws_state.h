/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Everything the /ws stream sends, and what every WebSocket session was last sent. Used only by
// ws.c; it makes no httpd or lwIP calls, so host tests can build it unchanged.
//
// Switch and Wi-Fi state are latest values with a generation number each. A session is owed a
// value when its generation is not 0 (known) and differs from the one last written to that
// session. A writer reads the generation first and the value after it, and marks the session
// with the generation it read, so a mark never covers older content.
//
// Events and sensor samples are held in two fixed lanes: an event ring, which evicts its oldest
// record when full, and the newest WS_SENSOR_SLOTS samples. Each open /ws session has a cursor
// into each lane, set at its handshake (no replay) and moved only after a write that carried
// the frames was accepted whole. A session whose cursor fell behind the ring is sent how many
// events it missed.
//
// Every function is non-blocking and may be called from any task, never from an ISR.

#ifndef ODROID_POWER_MATE_WS_STATE_H
#define ODROID_POWER_MATE_WS_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h" // TickType_t

// A WebSocket whose writes are skipped (not ready) for this long is closed. Longer than the
// page's 5 s pong timeout and pmtui's 15 s read deadline, so slow live clients survive.
#define WS_STALL_CLOSE_MS 20000
// A heap skip this long after the previous skip starts a new stall clock. Heap skips happen
// over HTTPS only, in the httpd task, which re-polls owed state every 200 ms but is held ~0.7 s
// by each TLS handshake, and handshakes are what drain the heap. This covers several of them
// back to back, so a continuous heap stall keeps its clock.
#define WS_HEAP_STALL_GAP_MS 5000

// The largest wifi_status StatusMessage is 66 B: connected, a 32 B SSID, a negative RSSI
// (10 B varint) and a 15 B address.
#define WS_WIFI_FRAME_MAX 80

// The event and sensor lanes. Their sizes are a static-RAM decision.
#define WS_EV_RING_SIZE 2048    // event bytes held; <= UINT16_MAX (offsets are uint16_t)
#define WS_EV_SLOTS 32          // events held at most; a power of two (seq % slots is wrap-safe)
#define WS_EVENT_FRAME_MAX 256  // == PB_BUFFER_SIZE (asserted in ws.c)
#define WS_SENSOR_SLOTS 3       // newest samples held
#define WS_SENSOR_FRAME_MAX 105 // == 2 + SensorData_size (asserted in ws.c)

/** Bumps the switch generation (skipping 0) and returns it. Call after storing the outputs. */
uint32_t ws_state_bump_sw(void);
/** Stores an encoded wifi_status frame and bumps its generation. False (nothing stored) if it is
 *  empty or longer than WS_WIFI_FRAME_MAX. */
bool ws_state_store_wifi(const uint8_t* frame, size_t len);
uint32_t ws_state_sw_gen(void);
uint32_t ws_state_wifi_gen(void);
/** Copies the Wi-Fi frame and its generation as one consistent pair. Returns its length, or 0
 *  when there is none yet or it does not fit. */
size_t ws_state_copy_wifi(uint8_t* buf, size_t size, uint32_t* gen);

/** The table slot of a socket. False for an fd outside lwIP's range, which is never tracked. */
bool ws_state_slot(int fd, int* slot);
/** The httpd session context of a /ws session on fd: its table entry, which ws.c treats as an
 *  opaque pointer. NULL for an fd outside the table. */
void* ws_state_ctx(int fd);
/** True if ctx is the /ws session context of fd. */
bool ws_state_ctx_is(int fd, const void* ctx);
/** A WebSocket handshake on fd: a new session, owed all state, with every flag cleared. status:
 *  a /ws session, which is open from now until its free_ctx, with its lane cursors at the
 *  newest event and sample; otherwise (/uart) it is never open. */
void ws_state_session_opened(int fd, bool status);
/** httpd's free_ctx of a /ws session: it is no longer open, and nothing for it is written or
 *  marked any more. Ignores any pointer other than a ws_state_ctx(), and a second call. */
void ws_state_ctx_free(void* ctx);
/** A write failed: write nothing more to this session. Set before its close is triggered. */
void ws_state_block(int fd);
/** Clears the blocked flag only (a plain GET of a WebSocket URI, which never upgrades). */
void ws_state_unblock(int fd);
/** True if blocked, and for an fd outside the table. */
bool ws_state_blocked(int fd);
/** Closing the session failed synchronously: a later state pass retries it. */
void ws_state_close_failed(int fd);
/** Test-and-clear of that retry flag: only one caller gets true. */
bool ws_state_take_close_retry(int fd);
/** A write to fd was skipped as not ready (heap: only for the TLS heap). Starts the stall clock
 *  unless it runs; a heap skip WS_HEAP_STALL_GAP_MS after the previous skip restarts it. */
void ws_state_write_skipped(int fd, TickType_t now, bool heap);
/** A write to fd succeeded: the stall is over. */
void ws_state_write_done(int fd);
/** True if fd has been stalled for at least ticks. */
bool ws_state_stalled_for(int fd, TickType_t now, TickType_t ticks);
/** True once fd has been stalled for WS_STALL_CLOSE_MS. *heap: whether the last skip was for
 *  the heap only. */
bool ws_state_stall_expired(int fd, TickType_t now, bool* heap);

struct ws_owed
{
    uint32_t epoch; // the session the answer is for: pass it to the marks
    bool sw;        // owed sw_status
    bool wifi;      // owed wifi_status
    bool snapshot;  // owed sw_status and never written state: state goes ahead of everything
    bool events;    // events are pending
    bool sensor;    // sensor samples are pending
};

/** What the session on fd is owed. Only an open /ws session is owed anything. Returns true if
 *  it is owed anything. */
bool ws_state_owed(int fd, struct ws_owed* o);
/** Records that generation gen was written, unless the fd has had a new session since epoch. */
void ws_state_mark_sw(int fd, uint32_t epoch, uint32_t gen);
void ws_state_mark_wifi(int fd, uint32_t epoch, uint32_t gen);

enum ws_push
{
    WS_PUSH_STORED,     // stored, and a /ws session is open: request a pass
    WS_PUSH_NO_SESSION, // stored, but no /ws session is open: no one to send it to
    WS_PUSH_REFUSED,    // empty or too long: not stored
};

/** Stores an event frame (at most WS_EVENT_FRAME_MAX), evicting the oldest events as needed. */
enum ws_push ws_state_push_event(const uint8_t* frame, size_t len);
/** Stores a sensor frame (at most WS_SENSOR_FRAME_MAX) over the oldest sample. */
enum ws_push ws_state_push_sensor(const uint8_t* frame, size_t len);
/** The seq the next event gets (events stored since boot). */
uint32_t ws_state_ev_head(void);

/** What one batch for a session is built from, read in one critical section. Seqs are
 *  [from, end); a cursor behind what is held starts at the oldest held, and the rest is missed
 *  (events) or merged (sensor samples). */
struct ws_lane_view
{
    uint32_t epoch;
    uint32_t ev_cursor;     // the session's event cursor
    uint32_t ev_from;       // the cursor, or the oldest held event if the cursor fell behind it
    uint32_t ev_end;        // ev_head
    uint32_t ev_missed;     // ev_from - ev_cursor: evicted before the session got them
    uint32_t sensor_from;   // the cursor, or the oldest held sample
    uint32_t sensor_end;    // samples stored since boot
    uint32_t sensor_merged; // sensor_from - the cursor: overwritten before the session got them
};

/** The lanes as the open /ws session on fd sees them. False (nothing to send) unless it is open.
 *  Moves no cursor. */
bool ws_state_lane_view(int fd, struct ws_lane_view* v);
/** Copies event seq into buf. Returns its length, or 0 with *gone if it was evicted, or 0 if it
 *  is not stored yet or longer than size. */
size_t ws_state_copy_event(uint32_t seq, uint8_t* buf, size_t size, bool* gone);
/** Copies sensor sample seq into buf. Returns its length, or 0 if it is no longer held (or not
 *  yet stored) or longer than size. */
size_t ws_state_copy_sensor(uint32_t seq, uint8_t* buf, size_t size);
/** A write that carried the frames up to these cursors was accepted whole. missed: events a
 *  marker in that write reported; merged: samples skipped. Ignored (false) unless the fd still
 *  holds the open session of epoch. */
bool ws_state_lane_commit(int fd, uint32_t epoch, uint32_t ev_next, uint32_t missed, uint32_t sensor_next,
                          uint32_t merged);

// Counts since boot. "Per session" counts add up over every /ws session.
struct ws_lane_stats
{
    uint32_t ev_no_session;        // events stored while no /ws session was open
    uint32_t ev_stored;            // events stored
    uint32_t events_missed;        // per session: evicted before it got them; reported by its markers
    uint32_t events_lost_on_close; // per session: not delivered when it ended (any reason)
    uint32_t sensor_merged;        // per session: samples skipped, more were pending than held
    uint32_t backlog_max;          // the most events one session had pending at a view (evicted ones too)
    uint32_t backlog_max_bytes;    // the most bytes of held events one session had pending at a view
};

void ws_state_lane_stats(struct ws_lane_stats* s);

/** A state pass must run (producer to status sender). */
void ws_state_request_pass(void);
bool ws_state_take_pass_request(void);

// HTTPS: the state pass runs in the httpd task as one work item at a time.
enum ws_kick
{
    WS_KICK_QUEUED,      // a work item was queued
    WS_KICK_OUTSTANDING, // one is queued and has not started: it will read the newest state
    WS_KICK_FAILED,      // queue() failed: nothing is queued
};

/** Queues the state work item through queue(ctx) unless one is already queued and not started.
 *  The flag is taken before queue() is called, so a work item that runs before queue() returns
 *  cannot leave it set. */
enum ws_kick ws_state_kick(bool (*queue)(void* ctx), void* ctx);
bool ws_state_kick_outstanding(void);
/** Forgets an outstanding kick (its work item was lost). */
void ws_state_kick_reset(void);
/** First statement of the work item, before it reads any generation or the table. */
void ws_state_pass_started(void);
/** The work item reports its result, the re-poll interval in ms (0: none), to the sender. */
void ws_state_report_pass(uint32_t ms);
bool ws_state_take_report(uint32_t* ms);

#endif // ODROID_POWER_MATE_WS_STATE_H
