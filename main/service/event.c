//
// Created by shinys on 26. 2. 20..
//

#include "event.h"

#include <sys/time.h>

#include "esp_timer.h"

void push_event(enum event_level level, char *msg_str)
{
    StatusMessage message = StatusMessage_init_zero;
    message.which_payload = StatusMessage_event_data_tag;
    EventData* event_data = &message.payload.event_data;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t timestamp_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
    uint64_t uptime_ms = (uint64_t)esp_timer_get_time() / 1000;

    event_data->level = level;
    event_data->timestamp_ms = timestamp_ms;
    event_data->uptime_ms = uptime_ms;
    event_data->message.funcs.encode = &encode_string;
    event_data->message.arg = msg_str;

    send_pb_message(StatusMessage_fields, &message);
}

void push_eventf(enum event_level level, char *format, ...)
{
    char buf[255];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    push_event(level, buf);
}