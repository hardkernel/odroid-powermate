//
// Created by shinys on 26. 2. 20..
//

#ifndef ODROID_POWER_MATE_PB_H
#define ODROID_POWER_MATE_PB_H

#define PB_BUFFER_SIZE 256

#include <stdbool.h>

#include "esp_log.h"

#include "pb_encode.h"
#include "pb.h"
#include "status.pb.h"
#include "webserver.h"

bool encode_string(pb_ostream_t* stream, const pb_field_t* field, void* const* arg);
void send_pb_message(const pb_msgdesc_t* fields, const void* src_struct);

#endif // ODROID_POWER_MATE_PB_H
