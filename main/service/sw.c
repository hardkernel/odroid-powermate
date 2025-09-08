//
// Created by vl011 on 2025-08-28.
//

#include "sw.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ina3221.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pb.h"
#include "pb_encode.h"
#include "pca9557.h"
#include "status.pb.h"
#include "webserver.h"

#define I2C_PORT 0

#define GPIO_SDA CONFIG_I2C_GPIO_SDA
#define GPIO_SCL CONFIG_I2C_GPIO_SCL
#define GPIO_MAIN CONFIG_EXPANDER_GPIO_SW_12V
#define GPIO_USB CONFIG_EXPANDER_GPIO_SW_5V
#define GPIO_PWR CONFIG_EXPANDER_GPIO_TRIGGER_POWER
#define GPIO_RST CONFIG_EXPANDER_GPIO_TRIGGER_RESET

#define POWER_DELAY (CONFIG_TRIGGER_POWER_DELAY_MS * 1000)
#define RESET_DELAY (CONFIG_TRIGGER_RESET_DELAY_MS * 1000)

#define PB_BUFFER_SIZE 256

static const char* TAG = "control";

static bool load_switch_12v_status = false;
static bool load_switch_5v_status = false;

static SemaphoreHandle_t expander_mutex;
#define MUTEX_TIMEOUT (pdMS_TO_TICKS(100))

static i2c_dev_t pca = {0};

static esp_timer_handle_t power_trigger_timer;
static esp_timer_handle_t reset_trigger_timer;

static void send_sw_status_message()
{
    StatusMessage message = StatusMessage_init_zero;
    message.which_payload = StatusMessage_sw_status_tag;
    LoadSwStatus* sw_status = &message.payload.sw_status;

    sw_status->main = load_switch_12v_status;
    sw_status->usb = load_switch_5v_status;

    uint8_t buffer[PB_BUFFER_SIZE];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, StatusMessage_fields, &message))
    {
        ESP_LOGE(TAG, "Failed to encode protobuf message: %s", PB_GET_ERROR(&stream));
        return;
    }

    push_data_to_ws(buffer, stream.bytes_written);
}


static void trigger_off_callback(void* arg)
{
    if (xSemaphoreTake(expander_mutex, MUTEX_TIMEOUT) == pdFALSE)
    {
        ESP_LOGW(TAG, "Control error");
        return;
    }

    uint32_t gpio_pin = (int)arg;
    pca9557_set_level(&pca, gpio_pin, 1);
    xSemaphoreGive(expander_mutex);
}

void config_sw()
{
    ESP_ERROR_CHECK(pca9557_set_mode(&pca, GPIO_MAIN, PCA9557_MODE_OUTPUT));
    ESP_ERROR_CHECK(pca9557_set_mode(&pca, GPIO_USB, PCA9557_MODE_OUTPUT));
    ESP_ERROR_CHECK(pca9557_set_mode(&pca, GPIO_PWR, PCA9557_MODE_OUTPUT));
    ESP_ERROR_CHECK(pca9557_set_mode(&pca, GPIO_RST, PCA9557_MODE_OUTPUT));

    ESP_ERROR_CHECK(pca9557_set_level(&pca, GPIO_PWR, 1));
    ESP_ERROR_CHECK(pca9557_set_level(&pca, GPIO_RST, 1));

    uint32_t val = 0;
    ESP_ERROR_CHECK(pca9557_get_level(&pca, CONFIG_EXPANDER_GPIO_SW_12V, &val));
    load_switch_12v_status = val != 0 ? true : false;
    ESP_ERROR_CHECK(pca9557_get_level(&pca, CONFIG_EXPANDER_GPIO_SW_5V, &val));
    load_switch_5v_status = val != 0 ? true : false;

    send_sw_status_message();
}

void init_sw()
{
    ESP_ERROR_CHECK(pca9557_init_desc(&pca, 0x18, I2C_PORT, GPIO_SDA, GPIO_SCL));

    config_sw();

    const esp_timer_create_args_t power_timer_args = {
        .callback = &trigger_off_callback, .arg = (void*)GPIO_PWR, .name = "power_trigger_off"};
    ESP_ERROR_CHECK(esp_timer_create(&power_timer_args, &power_trigger_timer));

    const esp_timer_create_args_t reset_timer_args = {
        .callback = &trigger_off_callback, .arg = (void*)GPIO_RST, .name = "power_trigger_off"};
    ESP_ERROR_CHECK(esp_timer_create(&reset_timer_args, &reset_trigger_timer));

    expander_mutex = xSemaphoreCreateMutex();
}

void trig_power()
{
    ESP_LOGI(TAG, "Trig power");
    if (xSemaphoreTake(expander_mutex, MUTEX_TIMEOUT) == pdFALSE)
    {
        ESP_LOGW(TAG, "Control error");
        return;
    }
    pca9557_set_level(&pca, GPIO_PWR, 0);
    xSemaphoreGive(expander_mutex);
    esp_timer_stop(power_trigger_timer);
    esp_timer_start_once(power_trigger_timer, POWER_DELAY);
}

void trig_reset()
{
    ESP_LOGI(TAG, "Trig reset");
    if (xSemaphoreTake(expander_mutex, MUTEX_TIMEOUT) == pdFALSE)
    {
        ESP_LOGW(TAG, "Control error");
        return;
    }
    pca9557_set_level(&pca, GPIO_RST, 0);
    xSemaphoreGive(expander_mutex);
    esp_timer_stop(reset_trigger_timer);
    esp_timer_start_once(reset_trigger_timer, RESET_DELAY);
}

void set_main_load_switch(bool on)
{
    ESP_LOGI(TAG, "Set main load switch to %s", on ? "on" : "off");
    if (load_switch_12v_status == on)
        return;
    if (xSemaphoreTake(expander_mutex, MUTEX_TIMEOUT) == pdFALSE)
    {
        ESP_LOGW(TAG, "Control error");
        return;
    }
    pca9557_set_level(&pca, GPIO_MAIN, on);
    load_switch_12v_status = on;
    xSemaphoreGive(expander_mutex);
    send_sw_status_message();
}

void set_usb_load_switch(bool on)
{
    ESP_LOGI(TAG, "Set usb load switch to %s", on ? "on" : "off");
    if (load_switch_5v_status == on)
        return;
    if (xSemaphoreTake(expander_mutex, MUTEX_TIMEOUT) == pdFALSE)
    {
        ESP_LOGW(TAG, "Control error");
        return;
    }
    pca9557_set_level(&pca, GPIO_USB, on);
    load_switch_5v_status = on;
    xSemaphoreGive(expander_mutex);
    send_sw_status_message();
}

bool get_main_load_switch() { return load_switch_12v_status; }

bool get_usb_load_switch() { return load_switch_5v_status; }
