/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Switch and Wi-Fi state for the /ws stream, kept as latest values with a generation
// number each, and what every WebSocket session was last sent. Used only by ws.c; it makes
// no httpd or lwIP calls, so host tests can build it unchanged.
//
// A session is owed a value when its generation is not 0 (known) and differs from the one
// last written to that session. A writer reads the generation first and the value after it,
// and marks the session with the generation it read, so a mark never covers older content.
//
// Every function is non-blocking and may be called from any task, never from an ISR.

#ifndef ODROID_POWER_MATE_WS_STATE_H
#define ODROID_POWER_MATE_WS_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The largest wifi_status StatusMessage is 66 B: connected, a 32 B SSID, a negative RSSI
// (10 B varint) and a 15 B address.
#define WS_WIFI_FRAME_MAX 80

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
/** A WebSocket handshake on fd: a new session, owed everything, with every flag cleared. */
void ws_state_session_opened(int fd);
/** A write failed: write nothing more to this session. Set before its close is triggered. */
void ws_state_block(int fd);
/** Clears the blocked flag only (a plain GET of a WebSocket URI, which never upgrades). */
void ws_state_unblock(int fd);
/** True if blocked, and for an fd outside the table. */
bool ws_state_blocked(int fd);

struct ws_owed
{
    uint32_t epoch; // the session the answer is for: pass it to the marks
    bool sw;        // owed sw_status
    bool wifi;      // owed wifi_status
};

/** What the session on fd is owed. Returns o->sw || o->wifi. */
bool ws_state_owed(int fd, struct ws_owed* o);
/** Records that generation gen was written, unless the fd has had a new session since epoch. */
void ws_state_mark_sw(int fd, uint32_t epoch, uint32_t gen);
void ws_state_mark_wifi(int fd, uint32_t epoch, uint32_t gen);

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
