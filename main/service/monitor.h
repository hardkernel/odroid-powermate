//
// Created by shinys on 25. 8. 18..
//

#ifndef ODROID_REMOTE_HTTP_MONITOR_H
#define ODROID_REMOTE_HTTP_MONITOR_H

#include <stdint.h>

#include "esp_http_server.h"

#define SENSOR_BUFFER_SIZE 100

typedef struct
{
    float voltage;
    float current;
    float power;
    uint32_t timestamp;
} sensor_data_t;

void init_status_monitor();
esp_err_t update_sensor_period(int period);

#endif // ODROID_REMOTE_HTTP_MONITOR_H
