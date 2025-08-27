//
// Created by shinys on 25. 8. 18..
//


#include "monitor.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "esp_wifi_types_generic.h"
#include "ina226.h"
#include "webserver.h"
#include "wifi.h"
#include "datalog.h"

#define INA226_SDA CONFIG_GPIO_INA226_SDA
#define INA226_SCL CONFIG_GPIO_INA226_SCL

ina226_t ina;
i2c_master_bus_handle_t bus_handle;
i2c_master_dev_handle_t dev_handle;

// Timer callback function to read sensor data
static void sensor_timer_callback(void *arg)
{
    // Generate random sensor data
    float voltage = 0;
    float current = 0;
    float power = 0;

    ina226_get_bus_voltage(&ina, &voltage);
    ina226_get_power(&ina, &power);
    ina226_get_current(&ina, &current);

    // Get system uptime
    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_sec = (uint32_t)(uptime_us / 1000000);
    uint32_t timestamp = (uint32_t)time(NULL);

    datalog_add(timestamp, voltage, current, power);

    // Create JSON object with sensor data
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "sensor_data");
    cJSON_AddNumberToObject(root, "voltage", voltage);
    cJSON_AddNumberToObject(root, "current", current);
    cJSON_AddNumberToObject(root, "power", power);
    cJSON_AddNumberToObject(root, "timestamp", timestamp);
    cJSON_AddNumberToObject(root, "uptime_sec", uptime_sec);

    // Push data to WebSocket clients
    push_data_to_ws(root);
}

static void status_wifi_callback(void *arg)
{
    wifi_ap_record_t ap_info;
    cJSON *root = cJSON_CreateObject();

    if (wifi_get_current_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(root, "type", "wifi_status");
        cJSON_AddBoolToObject(root, "connected", true);
        cJSON_AddStringToObject(root, "ssid", (const char *)ap_info.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
    } else {
        cJSON_AddBoolToObject(root, "connected", false);
    }

    push_data_to_ws(root);
}

ina226_config_t ina_config = {
    .i2c_port = I2C_NUM_0,
    .i2c_addr = 0x40,
    .timeout_ms = 100,
    .averages = INA226_AVERAGES_16,
    .bus_conv_time = INA226_BUS_CONV_TIME_1100_US,
    .shunt_conv_time = INA226_SHUNT_CONV_TIME_1100_US,
    .mode = INA226_MODE_SHUNT_BUS_CONT,
    .r_shunt = 0.01f,
    .max_current = 8
};

static void init_ina226()
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t) INA226_SDA,
        .scl_io_num = (gpio_num_t) INA226_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x40,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));

    ESP_ERROR_CHECK(ina226_init(&ina, dev_handle, &ina_config));
}

static esp_timer_handle_t sensor_timer;
static esp_timer_handle_t wifi_status_timer;

void init_status_monitor()
{
    init_ina226();
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
