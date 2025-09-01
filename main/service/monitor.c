//
// Created by shinys on 25. 8. 18..
//


#include "monitor.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "esp_wifi_types_generic.h"
#include "webserver.h"
#include "wifi.h"
#include "datalog.h"
#include "ina3221.h"

#define PM_SDA CONFIG_I2C_GPIO_SDA
#define PM_SCL CONFIG_I2C_GPIO_SCL

const char* channel_names[] = {
    "USB",
    "MAIN",
    "VIN"
};

ina3221_t ina3221 =
{
    /* shunt values are 100 mOhm for each channel */
    .shunt = {
        10,
        10,
        10
    },
    .mask.mask_register = INA3221_DEFAULT_MASK,
    .i2c_dev = {0},
    .config = {
        .mode = true, // mode selection
        .esht = true, // shunt enable
        .ebus = true, // bus enable
        .ch1 = true, // channel 1 enable
        .ch2 = true, // channel 2 enable
        .ch3 = true, // channel 3 enable
        .avg = INA3221_AVG_64, // 64 samples average
        .vbus = INA3221_CT_2116, // 2ms by channel (bus)
        .vsht = INA3221_CT_2116, // 2ms by channel (shunt)
    },
};

// Timer callback function to read sensor data
static void sensor_timer_callback(void* arg)
{
    // Get system uptime
    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_sec = (uint32_t)(uptime_us / 1000000);
    uint32_t timestamp = (uint32_t)time(NULL);

    channel_data_t channel_data[NUM_CHANNELS];

    // Create JSON object with sensor data
    cJSON* root = cJSON_CreateObject();
    for (uint8_t i = 0; i < INA3221_BUS_NUMBER; i++)
    {
        float voltage, current, power;

        ina3221_get_bus_voltage(&ina3221, i, &voltage);
        ina3221_get_shunt_value(&ina3221, i, NULL, &current);

        current /= 1000.0f; // mA to A
        power = voltage * current;

        // Populate data for datalog
        channel_data[i].voltage = voltage;
        channel_data[i].current = current;
        channel_data[i].power = power;

        // Populate data for websocket
        cJSON* v = cJSON_AddObjectToObject(root, channel_names[i]);
        cJSON_AddNumberToObject(v, "voltage", voltage);
        cJSON_AddNumberToObject(v, "current", current);
        cJSON_AddNumberToObject(v, "power", power);
    }

    // Add data to log file
    datalog_add(timestamp, channel_data);

    cJSON_AddStringToObject(root, "type", "sensor_data");
    cJSON_AddNumberToObject(root, "timestamp", timestamp);
    cJSON_AddNumberToObject(root, "uptime_sec", uptime_sec);

    // Push data to WebSocket clients
    push_data_to_ws(root);
}

static void status_wifi_callback(void* arg)
{
    wifi_ap_record_t ap_info;
    cJSON* root = cJSON_CreateObject();

    if (wifi_get_current_ap_info(&ap_info) == ESP_OK)
    {
        cJSON_AddStringToObject(root, "type", "wifi_status");
        cJSON_AddBoolToObject(root, "connected", true);
        cJSON_AddStringToObject(root, "ssid", (const char*)ap_info.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
    }
    else
    {
        cJSON_AddBoolToObject(root, "connected", false);
    }

    push_data_to_ws(root);
}

static esp_timer_handle_t sensor_timer;
static esp_timer_handle_t wifi_status_timer;

void init_status_monitor()
{
    ESP_ERROR_CHECK(ina3221_init_desc(&ina3221, 0x40, 0, PM_SDA, PM_SCL));

    // logger
    datalog_init();

    // Timer configuration
    const esp_timer_create_args_t sensor_timer_args = {
        .callback = &sensor_timer_callback,
        .name = "sensor_reading_timer" // Optional name for debugging
    };

    const esp_timer_create_args_t wifi_timer_args = {
        .callback = &status_wifi_callback,
        .name = "wifi_status_timer" // Optional name for debugging
    };

    ESP_ERROR_CHECK(esp_timer_create(&sensor_timer_args, &sensor_timer));
    ESP_ERROR_CHECK(esp_timer_create(&wifi_timer_args, &wifi_status_timer));

    ESP_ERROR_CHECK(esp_timer_start_periodic(sensor_timer, 1000000)); // 1sec
    ESP_ERROR_CHECK(esp_timer_start_periodic(wifi_status_timer, 1000000 * 5)); // 5s in microseconds
}
