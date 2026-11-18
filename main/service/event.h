/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 26. 2. 20..
//

#ifndef ODROID_POWER_MATE_EVENT_H
#define ODROID_POWER_MATE_EVENT_H

#include "pbmsg.h"

enum event_level
{
    EV_INFO = 0,
    EV_WARNING = 1,
    EV_CRITICAL = 2,
    EV_FATAL = 3,
};


/** Encodes an EventData StatusMessage stamped now into buf. Returns its length, or 0 (logged) if
 *  it does not fit. */
size_t encode_event(uint8_t* buf, size_t size, enum event_level level, const char* msg);
void push_event(enum event_level level, char *msg_str);
void push_eventf(enum event_level level, char *format, ...) __attribute__((format(printf, 2, 3)));


#endif // ODROID_POWER_MATE_EVENT_H
