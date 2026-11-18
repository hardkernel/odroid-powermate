/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 26. 2. 20..
//

#include "pbmsg.h"

static const char *TAG = "msg";

bool encode_string(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const char* str = (const char*)(*arg);
    if (!str)
    {
        return true; // Nothing to encode
    }
    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }
    return pb_encode_string(stream, (uint8_t*)str, strlen(str));
}

size_t encode_pb_message(const pb_msgdesc_t* fields, const void* src_struct, uint8_t* buf, size_t size)
{
    pb_ostream_t stream = pb_ostream_from_buffer(buf, size);

    if (!pb_encode(&stream, fields, src_struct))
    {
        ESP_LOGE(TAG, "Failed to encode protobuf message: %s", PB_GET_ERROR(&stream));
        return 0;
    }
    return stream.bytes_written;
}

void send_pb_message(const pb_msgdesc_t* fields, const void* src_struct)
{
    uint8_t buffer[PB_BUFFER_SIZE];
    size_t len = encode_pb_message(fields, src_struct, buffer, sizeof(buffer));
    if (len)
        push_data_to_ws(buffer, len);
}