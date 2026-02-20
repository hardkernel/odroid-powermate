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


void push_event(enum event_level level, char *msg_str);
void push_eventf(enum event_level level, char *format, ...) __attribute__((format(printf, 2, 3)));


#endif // ODROID_POWER_MATE_EVENT_H
