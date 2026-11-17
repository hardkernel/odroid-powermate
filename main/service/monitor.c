/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 25. 8. 18..
//

#include "monitor.h"
#include <nconfig.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "climit.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi_types_generic.h"
#include "event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "ina3221.h"
#include "pbmsg.h"
#include "sw.h"
#include "webserver.h"
#include "wifi.h"

#define CHANNEL_VIN 2  // IN3
#define CHANNEL_MAIN 1 // IN2
#define CHANNEL_USB 0  // IN1


#define PM_INT_CRITICAL CONFIG_GPIO_INA3221_INT_CRITICAL
#define PM_INT_WARNING CONFIG_GPIO_INA3221_INT_WARNING
#define PM_EXPANDER_RST CONFIG_GPIO_EXPANDER_RESET

#define CLIMIT_DISABLED_LIMIT_A 15.0f
#define CLIMIT_VERIFY_TOLERANCE_A 0.01f

static const char* TAG = "monitor";

static esp_timer_handle_t sensor_timer;
static esp_timer_handle_t wifi_status_timer;
static esp_timer_handle_t long_press_timer;
// static esp_timer_handle_t shutdown_load_sw; // No longer needed

static TaskHandle_t shutdown_task_handle = NULL; // Global task handle
static TaskHandle_t warning_task_handle = NULL;
static volatile bool critical_cutoff_pending = false;
static volatile uint16_t pending_critical_flags = 0;
static volatile uint16_t pending_warning_flags = 0;
static portMUX_TYPE pending_flags_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t status_lock;
static uint16_t last_warning_flags = 0;

#define INA3221_ADDR 0x40

// Shunt resistors, in milliohms.
static const uint16_t shunt_mohm[INA3221_CHANNELS] = {10, 10, 10};

// All channels, continuous shunt and bus conversions, 16-sample averaging,
// 140 us bus and 1.1 ms shunt conversion time. One averaged result per channel every
// 3 x (140 + 1100) us x 16 = 59.5 ms. The warning alert compares these averages, so this
// window is also the warning cut-off delay; the critical alert checks every single
// conversion. A shunt conversion takes 1.1 of every 3.72 ms, near the 1/3 maximum for
// three channels sharing one ADC.
#define INA3221_CYCLE_US (3 * (140 + 1100) * 16)
static const uint16_t ina3221_config = INA3221_CONFIG_CH_EN(0) | INA3221_CONFIG_CH_EN(1) | INA3221_CONFIG_CH_EN(2) |
    INA3221_CONFIG_AVG(INA3221_AVG_16) | INA3221_CONFIG_VBUS_CT(INA3221_CT_140US) |
    INA3221_CONFIG_VSH_CT(INA3221_CT_1100US) | INA3221_CONFIG_MODE_CONTINUOUS;

typedef struct
{
    const char* name;
    uint8_t channel;
    uint16_t cf_bit;
} monitor_channel_t;

static const monitor_channel_t monitor_channels[] = {
    {.name = "USB", .channel = CHANNEL_USB, .cf_bit = BIT2},   // IN1
    {.name = "MAIN", .channel = CHANNEL_MAIN, .cf_bit = BIT1}, // IN2
    {.name = "VIN", .channel = CHANNEL_VIN, .cf_bit = BIT0},   // IN3
};

static void add_pending_flags(volatile uint16_t* flags, uint16_t value)
{
    taskENTER_CRITICAL(&pending_flags_mux);
    *flags |= value;
    taskEXIT_CRITICAL(&pending_flags_mux);
}

static uint16_t take_pending_flags(volatile uint16_t* flags)
{
    taskENTER_CRITICAL(&pending_flags_mux);
    uint16_t value = *flags;
    *flags = 0;
    taskEXIT_CRITICAL(&pending_flags_mux);
    return value;
}

/**
 * Reads the Mask/Enable register. *mask is valid only on ESP_OK.
 *
 * The read clears the latched critical flags, and the sampler reads this register every
 * conversion cycle. So every read parks the critical flags in pending_critical_flags,
 * under one lock, before anyone else can read the register again: the shutdown task reads
 * the status first and takes the pending flags after, and never sees a cleared source.
 * The warning flags are not latched here, but the sampler still forwards the ones it sees
 * newly set (see sampler_task), so a read there can never hide a warning from its task.
 */
static esp_err_t ina3221_get_status(uint16_t* mask)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    esp_err_t err = ina3221_read(INA3221_REG_MASK, mask);
    if (err == ESP_OK)
        add_pending_flags(&pending_critical_flags, INA3221_MASK_CF(*mask));
    xSemaphoreGive(status_lock);
    return err;
}

static void notify_alert_tasks(uint16_t cf, uint16_t wf)
{
    if (cf)
    {
        add_pending_flags(&pending_critical_flags, cf);
        if (shutdown_task_handle != NULL)
            xTaskNotifyGive(shutdown_task_handle);
    }

    if (wf)
    {
        add_pending_flags(&pending_warning_flags, wf);
        if (warning_task_handle != NULL)
            xTaskNotifyGive(warning_task_handle);
    }
}

static float climit_raw_to_a(uint8_t channel, uint16_t raw)
{
    return ina3221_shunt_microvolts(raw) / shunt_mohm[channel] / 1000.0f;
}

static esp_err_t climit_get_channel_reg(uint8_t channel, uint8_t reg, float* limit_a, uint16_t* raw)
{
    uint16_t raw_value;
    esp_err_t err = ina3221_read(reg, &raw_value);
    if (err != ESP_OK)
        return err;

    if (raw)
        *raw = raw_value;
    if (limit_a)
        *limit_a = climit_raw_to_a(channel, raw_value);

    return ESP_OK;
}

static esp_err_t climit_get_channel(uint8_t channel, float* limit_a, uint16_t* raw)
{
    return climit_get_channel_reg(channel, INA3221_REG_WARNING(channel), limit_a, raw);
}

static esp_err_t climit_get_critical_channel(uint8_t channel, float* limit_a, uint16_t* raw)
{
    return climit_get_channel_reg(channel, INA3221_REG_CRITICAL(channel), limit_a, raw);
}

static esp_err_t climit_set_channel(const char* name, const char* label, uint8_t channel, double value,
                                    bool allow_disable)
{
    float requested_limit_a = (float)value;
    if (!allow_disable && requested_limit_a < CRITICAL_CURRENT_LIMIT_MIN)
    {
        ESP_LOGW(TAG, "%s %s request rejected: requested=%.3fA", name, label, requested_limit_a);
        push_eventf(EV_WARNING, "%s %s request rejected: requested=%.3fA", name, label, requested_limit_a);
        return ESP_ERR_INVALID_ARG;
    }

    float applied_limit_a = requested_limit_a > 0.0f ? requested_limit_a : CLIMIT_DISABLED_LIMIT_A;
    float applied_limit_ma = applied_limit_a * 1000.0f;
    bool is_critical = !strcmp(label, "critical current limit");

    ESP_LOGI(TAG, "Setting %s %s to: %fmA", name, label, requested_limit_a * 1000.0f);
    push_eventf(EV_INFO, "Setting %s %s to: %fmA", name, label, requested_limit_a * 1000.0f);

    // mA x mOhm = uV across the shunt.
    uint16_t limit_raw = ina3221_limit_raw(applied_limit_ma * shunt_mohm[channel]);
    esp_err_t err = ina3221_write(is_critical ? INA3221_REG_CRITICAL(channel) : INA3221_REG_WARNING(channel), limit_raw);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write %s %s: %s", name, label, esp_err_to_name(err));
        push_eventf(EV_WARNING, "%s %s set failed: requested=%.3fA, applied=%.3fA, error=%s", name, label,
                    requested_limit_a, applied_limit_a, esp_err_to_name(err));
        return err;
    }

    float readback_limit_a;
    err = is_critical ? climit_get_critical_channel(channel, &readback_limit_a, NULL)
                      : climit_get_channel(channel, &readback_limit_a, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read back %s %s: %s", name, label, esp_err_to_name(err));
        push_eventf(EV_WARNING, "%s %s readback failed: requested=%.3fA, applied=%.3fA, error=%s", name, label,
                    requested_limit_a, applied_limit_a, esp_err_to_name(err));
        return err;
    }

    float diff = readback_limit_a - applied_limit_a;
    if (diff < 0.0f)
        diff = -diff;

    ESP_LOGI(TAG, "%s %s set: applied=%.3fA, requested=%.3fA", name, label, readback_limit_a, requested_limit_a);
    push_eventf(EV_INFO, "%s %s set: applied=%.3fA, requested=%.3fA", name, label, readback_limit_a,
                requested_limit_a);

    if (diff > CLIMIT_VERIFY_TOLERANCE_A)
    {
        ESP_LOGE(TAG, "%s %s readback mismatch: expected=%.3fA, actual=%.3fA", name, label, applied_limit_a,
                 readback_limit_a);
        push_eventf(EV_WARNING, "%s %s readback mismatch: expected=%.3fA, actual=%.3fA", name, label,
                    applied_limit_a, readback_limit_a);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t mask_raw = 0;
    esp_err_t mask_err = ina3221_get_status(&mask_raw);
    if (mask_err == ESP_OK)
    {
        ESP_LOGI(TAG, "%s %s status: critical_gpio=%d warning_gpio=%d mask=0x%04x", name, label,
                 gpio_get_level(PM_INT_CRITICAL), gpio_get_level(PM_INT_WARNING), mask_raw);
    }

    return ESP_OK;
}

/** Latest averaged bus voltage (V) and current (A) of one channel. */
static esp_err_t read_channel(uint8_t channel, float* voltage, float* current_a)
{
    uint16_t bus, shunt;
    esp_err_t err = ina3221_read(INA3221_REG_BUS(channel), &bus);
    if (err == ESP_OK)
        err = ina3221_read(INA3221_REG_SHUNT(channel), &shunt);
    if (err != ESP_OK)
        return err;

    *voltage = ina3221_bus_volts(bus);
    *current_a = ina3221_shunt_microvolts(shunt) / shunt_mohm[channel] / 1000.0f; // uV / mOhm = mA
    return ESP_OK;
}

static esp_err_t read_channel_snapshot(const monitor_channel_t* channel, float* voltage, float* current_a)
{
    return read_channel(channel->channel, voltage, current_a);
}

static void push_critical_fault_sources(uint16_t cf)
{
    ESP_LOGW(TAG, "critical fault summary: cf=0x%x int_gpio=%d", cf, gpio_get_level(PM_INT_CRITICAL));

    for (size_t i = 0; i < sizeof(monitor_channels) / sizeof(monitor_channels[0]); ++i)
    {
        const monitor_channel_t* channel = &monitor_channels[i];
        if (cf & channel->cf_bit)
        {
            ESP_LOGW(TAG, "critical cutoff source: %s", channel->name);
            push_eventf(EV_CRITICAL, "critical cutoff source: %s", channel->name);
        }
    }
}

static void push_warning_fault_details(uint16_t wf)
{
    ESP_LOGW(TAG, "warning fault summary: wf=0x%x int_gpio=%d", wf, gpio_get_level(PM_INT_WARNING));

    bool any_channel_fault = false;
    for (size_t i = 0; i < sizeof(monitor_channels) / sizeof(monitor_channels[0]); ++i)
    {
        const monitor_channel_t* channel = &monitor_channels[i];
        bool flagged = (wf & channel->cf_bit) != 0;
        any_channel_fault = any_channel_fault || flagged;

        float voltage = 0.0f;
        float current_a = 0.0f;
        float limit_a = 0.0f;
        uint16_t raw_limit = 0;
        esp_err_t sample_err = read_channel_snapshot(channel, &voltage, &current_a);
        esp_err_t limit_err = climit_get_channel(channel->channel, &limit_a, &raw_limit);

        if (sample_err == ESP_OK && limit_err == ESP_OK)
        {
            ESP_LOGW(TAG, "warning detail: %s flagged=%d current=%.3fA voltage=%.3fV limit=%.3fA raw=0x%04x",
                     channel->name, flagged, current_a, voltage, limit_a, raw_limit);
            push_eventf(EV_WARNING, "warning detail: %s current=%.3fA voltage=%.3fV limit=%.3fA", channel->name,
                        current_a, voltage, limit_a);
        }
        else
        {
            ESP_LOGW(TAG, "warning detail: %s read failed: sample=%s, limit=%s", channel->name,
                     esp_err_to_name(sample_err), esp_err_to_name(limit_err));
            push_eventf(EV_WARNING, "warning detail: %s read failed: sample=%s, limit=%s", channel->name,
                        esp_err_to_name(sample_err), esp_err_to_name(limit_err));
        }
    }

    if (!any_channel_fault)
    {
        ESP_LOGW(TAG, "warning input asserted without INA3221 channel flag");
        push_eventf(EV_WARNING, "warning input asserted without INA3221 channel flag");
    }
}

static void disable_warning_fault_load_switches(uint16_t wf)
{
    if (wf & BIT0) // CH3 VIN input warning protects all downstream outputs.
    {
        ESP_LOGW(TAG, "warning action: VIN limit exceeded, disabling all load switches");
        push_eventf(EV_WARNING, "warning action: VIN limit exceeded, disabling all load switches");
        esp_err_t err = set_load_switches(false, false);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "warning action failed: all=%s", esp_err_to_name(err));
        return;
    }

    if (wf & BIT1) // CH2 MAIN
    {
        ESP_LOGW(TAG, "warning action: MAIN limit exceeded, disabling MAIN load switch");
        push_eventf(EV_WARNING, "warning action: MAIN limit exceeded, disabling MAIN load switch");
        esp_err_t err = set_main_load_switch(false);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "warning action failed: main=%s", esp_err_to_name(err));
    }

    if (wf & BIT2) // CH1 USB
    {
        ESP_LOGW(TAG, "warning action: USB limit exceeded, disabling USB load switch");
        push_eventf(EV_WARNING, "warning action: USB limit exceeded, disabling USB load switch");
        esp_err_t err = set_usb_load_switch(false);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "warning action failed: usb=%s", esp_err_to_name(err));
    }
}

static double clamp_current_limit(double value, double max_value)
{
    if (value < 0.0)
        return 0.0;
    if (value > max_value)
        return max_value;
    return value;
}

static double clamp_critical_current_limit(double value, double max_value)
{
    if (value < CRITICAL_CURRENT_LIMIT_MIN)
        return CRITICAL_CURRENT_LIMIT_MIN;
    if (value > max_value)
        return max_value;
    return value;
}

typedef struct
{
    float voltage_sum;
    float current_sum;
    float power_sum;
    float current_max;
    float power_max;
    uint32_t count;
} channel_accumulator_t;

// Every averaged result since the last report, so a report covers its whole period
// instead of the one 59.5 ms window that happened to be in the registers.
static channel_accumulator_t accumulators[INA3221_CHANNELS];
static portMUX_TYPE accumulator_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * The sampler's Mask/Enable read. The warning flags change when an averaged result is
 * ready, the very moment this task polls, and the warning task reads the register only
 * after it is notified. So newly set warning flags are handed to it here, whether or not
 * this read would have cleared them; flags that stay set are not sent again.
 */
static esp_err_t sampler_get_status(uint16_t* mask, uint16_t* last_wf)
{
    esp_err_t err = ina3221_get_status(mask);
    if (err != ESP_OK)
        return err;
    uint16_t wf = INA3221_MASK_WF(*mask);
    uint16_t new_wf = wf & ~*last_wf;
    *last_wf = wf;
    if (new_wf)
        notify_alert_tasks(0, new_wf);
    return ESP_OK;
}

static void sampler_task(void* arg)
{
    uint16_t last_wf = 0;
    while (1)
    {
        // Sleep through most of the cycle, then poll the conversion-ready flag.
        vTaskDelay(pdMS_TO_TICKS(INA3221_CYCLE_US / 1000 - 6));
        uint16_t mask = 0;
        for (int polls = 0; polls < 10; polls++)
        {
            if (sampler_get_status(&mask, &last_wf) != ESP_OK || (mask & INA3221_MASK_CVRF))
                break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        // Another reader of the Mask/Enable register (an alert task) can clear the flag
        // first; that cycle is skipped.
        if (!(mask & INA3221_MASK_CVRF))
            continue;

        float voltage[INA3221_CHANNELS], current[INA3221_CHANNELS];
        bool ok = true;
        for (uint8_t i = 0; i < INA3221_CHANNELS && ok; i++)
            ok = read_channel(i, &voltage[i], &current[i]) == ESP_OK;
        if (!ok)
            continue;

        taskENTER_CRITICAL(&accumulator_mux);
        for (uint8_t i = 0; i < INA3221_CHANNELS; i++)
        {
            channel_accumulator_t* acc = &accumulators[i];
            float power = voltage[i] * current[i];
            if (acc->count == 0 || current[i] > acc->current_max)
                acc->current_max = current[i];
            if (acc->count == 0 || power > acc->power_max)
                acc->power_max = power;
            acc->voltage_sum += voltage[i];
            acc->current_sum += current[i];
            acc->power_sum += power;
            acc->count++;
        }
        taskEXIT_CRITICAL(&accumulator_mux);
    }
}

static void sensor_timer_callback(void* arg)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t timestamp_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
    uint64_t uptime_ms = (uint64_t)esp_timer_get_time() / 1000;

    StatusMessage message = StatusMessage_init_zero;
    message.which_payload = StatusMessage_sensor_data_tag;
    SensorData* sensor_data = &message.payload.sensor_data;

    sensor_data->has_usb = true;
    sensor_data->has_main = true;
    sensor_data->has_vin = true;

    SensorChannelData* channels[] = {&sensor_data->usb, &sensor_data->main, &sensor_data->vin};

    channel_accumulator_t period[INA3221_CHANNELS];
    taskENTER_CRITICAL(&accumulator_mux);
    memcpy(period, accumulators, sizeof(period));
    memset(accumulators, 0, sizeof(accumulators));
    taskEXIT_CRITICAL(&accumulator_mux);

    for (uint8_t i = 0; i < INA3221_CHANNELS; i++)
    {
        const channel_accumulator_t* acc = &period[i];
        if (acc->count > 0)
        {
            // Mean of the averaged results in this period, power per result (mean of V x I).
            channels[i]->voltage = acc->voltage_sum / acc->count;
            channels[i]->current = acc->current_sum / acc->count;
            channels[i]->power = acc->power_sum / acc->count;
            channels[i]->current_max = acc->current_max;
            channels[i]->power_max = acc->power_max;
        }
        else
        {
            // No new result since the last report (sampler starting, or a skipped cycle).
            float voltage = 0.0f, current = 0.0f;
            read_channel(i, &voltage, &current);
            channels[i]->voltage = voltage;
            channels[i]->current = current;
            channels[i]->power = voltage * current;
            channels[i]->current_max = current;
            channels[i]->power_max = channels[i]->power;
        }
    }

    // datalog_add(timestamp, channel_data_log);

    sensor_data->timestamp_ms = timestamp_ms;
    sensor_data->uptime_ms = uptime_ms;

    send_pb_message(StatusMessage_fields, &message);
}

static void status_wifi_callback(void* arg)
{
    wifi_ap_record_t ap_info;
    StatusMessage message = StatusMessage_init_zero;
    message.which_payload = StatusMessage_wifi_status_tag;
    WifiStatus* wifi_status = &message.payload.wifi_status;
    char ip_str[16];
    esp_netif_ip_info_t ip_info;

    if (wifi_get_current_ap_info(&ap_info) == ESP_OK)
    {
        wifi_status->connected = true;
        wifi_status->ssid.funcs.encode = &encode_string;
        wifi_status->ssid.arg = (void*)ap_info.ssid;
        wifi_status->rssi = ap_info.rssi;
    }
    else
    {
        wifi_status->connected = false;
        wifi_status->ssid.arg = ""; // Empty string
        wifi_status->rssi = 0;
    }

    if (wifi_get_current_ip_info(&ip_info) == ESP_OK)
    {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        wifi_status->ip_address.funcs.encode = &encode_string;
        wifi_status->ip_address.arg = ip_str;
    }
    else
    {
        wifi_status->ip_address.arg = ""; // Empty string
    }

    send_pb_message(StatusMessage_fields, &message);
}

// Placeholder for long press action
static void handle_critical_long_press(void)
{
    ESP_LOGW(TAG, "Config reset triggered...");
    reset_nconfig();
}

// Timer callback for long press detection
static void long_press_timer_callback(void* arg)
{
    if (gpio_get_level(PM_INT_CRITICAL) == 0)
    {
        handle_critical_long_press();
    }
}

// New FreeRTOS task for shutdown logic
static void shutdown_load_sw_task(void* pvParameters)
{
    while (1)
    {
        // Wait indefinitely for a notification from the ISR
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // The critical ISR normally cuts the outputs first. Keep this as a
        // fallback for alerts forwarded by another task or detected at startup.
        gpio_set_level(PM_EXPANDER_RST, 0);

        ESP_LOGW(TAG, "critical interrupt triggered (via task)");
        uint16_t mask = 0;
        esp_err_t status_err = ina3221_get_status(&mask);
        // After the status read: it parks the latched flags, and so does every earlier read.
        uint16_t cf = take_pending_flags(&pending_critical_flags);
        bool button_pressed = false;

        if (status_err == ESP_OK)
        {
            cf |= INA3221_MASK_CF(mask); // BIT2=IN1/USB, BIT1=IN2/MAIN, BIT0=IN3/VIN
            notify_alert_tasks(0, INA3221_MASK_WF(mask));
            if (cf)
                push_critical_fault_sources(cf);
            else
            {
                push_eventf(EV_CRITICAL, "critical cutoff source: BTN");
                button_pressed = true;
            }
        }
        else
        {
            ESP_LOGW(TAG, "failed to read INA3221 status on critical interrupt: %s", esp_err_to_name(status_err));
            if (cf)
                push_critical_fault_sources(cf);
            else
                push_eventf(EV_CRITICAL, "critical cutoff source: unknown (INA3221 status read failed: %s)",
                            esp_err_to_name(status_err));
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_set_level(PM_EXPANDER_RST, 1);
        config_sw();
        esp_err_t persist_err = persist_load_switch_state();
        if (persist_err != ESP_OK)
            ESP_LOGW(TAG, "failed to save disabled load switch state: %s", esp_err_to_name(persist_err));

        push_eventf(EV_CRITICAL, "load switch disabled");

        // Only a held button press can trigger the configuration reset.
        if (button_pressed && gpio_get_level(PM_INT_CRITICAL) == 0)
        {
            esp_timer_start_once(long_press_timer, 5000000);
            // Close the race where the button is released just before the
            // timer becomes active and the rising-edge ISR has already run.
            if (gpio_get_level(PM_INT_CRITICAL) != 0)
                esp_timer_stop(long_press_timer);
        }
    }
}

static void warning_alert_task(void* pvParameters)
{
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG, "warning interrupt triggered (via task)");
        uint16_t wf = take_pending_flags(&pending_warning_flags);
        uint16_t mask = 0;
        esp_err_t status_err = ina3221_get_status(&mask);

        if (status_err == ESP_OK)
        {
            wf |= INA3221_MASK_WF(mask);
            notify_alert_tasks(INA3221_MASK_CF(mask), 0);
            if (wf)
            {
                push_warning_fault_details(wf);
                disable_warning_fault_load_switches(wf);
                last_warning_flags = wf;
            }
            else if (last_warning_flags && gpio_get_level(PM_INT_WARNING) != 0)
            {
                ESP_LOGW(TAG, "warning cleared: previous_wf=0x%x int_gpio=%d", last_warning_flags,
                         gpio_get_level(PM_INT_WARNING));
                push_eventf(EV_WARNING, "warning cleared");
                last_warning_flags = 0;
            }
        }
        else
        {
            ESP_LOGW(TAG, "failed to read INA3221 status on warning interrupt: %s", esp_err_to_name(status_err));
            push_eventf(EV_WARNING, "warning fault: INA3221 status read failed: %s", esp_err_to_name(status_err));
        }
    }
}

static void IRAM_ATTR critical_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (gpio_get_level(PM_INT_CRITICAL) == 0) // Falling edge
    {
        // Critical current and button presses both require immediate cutoff.
        gpio_set_level(PM_EXPANDER_RST, 0);

        if (shutdown_task_handle != NULL)
        {
            vTaskNotifyGiveFromISR(shutdown_task_handle, &xHigherPriorityTaskWoken);
        }
        else
        {
            critical_cutoff_pending = true;
        }
    }
    else // Rising edge
    {
        // Stop the timer if the button is released
        esp_timer_stop(long_press_timer);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void IRAM_ATTR warning_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (warning_task_handle != NULL)
    {
        vTaskNotifyGiveFromISR(warning_task_handle, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void gpio_init()
{
    // Configure the cutoff output before enabling the critical interrupt.
    gpio_set_level(PM_EXPANDER_RST, 1);
    gpio_set_direction(PM_EXPANDER_RST, GPIO_MODE_OUTPUT);

    // critical int
    gpio_set_intr_type(PM_INT_CRITICAL, GPIO_INTR_ANYEDGE);
    gpio_set_direction(PM_INT_CRITICAL, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PM_INT_CRITICAL, GPIO_PULLUP_ONLY);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(PM_INT_CRITICAL, critical_isr_handler, (void*)PM_INT_CRITICAL);

    // warning int
    gpio_set_intr_type(PM_INT_WARNING, GPIO_INTR_ANYEDGE);
    gpio_set_direction(PM_INT_WARNING, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PM_INT_WARNING, GPIO_PULLUP_ONLY);
    gpio_isr_handler_add(PM_INT_WARNING, warning_isr_handler, (void*)PM_INT_WARNING);
}

esp_err_t climit_set_vin(double value)
{
    return climit_set_channel("VIN", "current limit", CHANNEL_VIN, value, true);
}

esp_err_t climit_set_main(double value)
{
    return climit_set_channel("MAIN", "current limit", CHANNEL_MAIN, value, true);
}

esp_err_t climit_set_usb(double value)
{
    return climit_set_channel("USB", "current limit", CHANNEL_USB, value, true);
}

esp_err_t climit_set_critical_vin(double value)
{
    return climit_set_channel("VIN", "critical current limit", CHANNEL_VIN, value, false);
}

esp_err_t climit_set_critical_main(double value)
{
    return climit_set_channel("MAIN", "critical current limit", CHANNEL_MAIN, value, false);
}

esp_err_t climit_set_critical_usb(double value)
{
    return climit_set_channel("USB", "critical current limit", CHANNEL_USB, value, false);
}

esp_err_t climit_get_vin(float* limit_a, uint16_t* raw)
{
    return climit_get_channel(CHANNEL_VIN, limit_a, raw);
}

esp_err_t climit_get_main(float* limit_a, uint16_t* raw)
{
    return climit_get_channel(CHANNEL_MAIN, limit_a, raw);
}

esp_err_t climit_get_usb(float* limit_a, uint16_t* raw)
{
    return climit_get_channel(CHANNEL_USB, limit_a, raw);
}

esp_err_t climit_get_critical_vin(float* limit_a, uint16_t* raw)
{
    return climit_get_critical_channel(CHANNEL_VIN, limit_a, raw);
}

esp_err_t climit_get_critical_main(float* limit_a, uint16_t* raw)
{
    return climit_get_critical_channel(CHANNEL_MAIN, limit_a, raw);
}

esp_err_t climit_get_critical_usb(float* limit_a, uint16_t* raw)
{
    return climit_get_critical_channel(CHANNEL_USB, limit_a, raw);
}

void init_status_monitor()
{
    status_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(status_lock ? ESP_OK : ESP_ERR_NO_MEM);
    gpio_init();
    // Latch the critical alert so the shutdown task can still read which channel tripped.
    ESP_ERROR_CHECK(ina3221_init(INA3221_ADDR, ina3221_config, INA3221_MASK_CEN));

    double lim;
    char buf[16];

    nconfig_read(VIN_CURRENT_LIMIT, buf, sizeof(buf));
    lim = clamp_current_limit(atof(buf), VIN_CURRENT_LIMIT_MAX);
    climit_set_vin(lim);
    nconfig_read(VIN_CRITICAL_CURRENT_LIMIT, buf, sizeof(buf));
    lim = clamp_critical_current_limit(atof(buf), VIN_CRITICAL_CURRENT_LIMIT_MAX);
    climit_set_critical_vin(lim);

    nconfig_read(MAIN_CURRENT_LIMIT, buf, sizeof(buf));
    lim = clamp_current_limit(atof(buf), MAIN_CURRENT_LIMIT_MAX);
    climit_set_main(lim);
    nconfig_read(MAIN_CRITICAL_CURRENT_LIMIT, buf, sizeof(buf));
    lim = clamp_critical_current_limit(atof(buf), MAIN_CRITICAL_CURRENT_LIMIT_MAX);
    climit_set_critical_main(lim);

    nconfig_read(USB_CURRENT_LIMIT, buf, sizeof(buf));
    lim = clamp_current_limit(atof(buf), USB_CURRENT_LIMIT_MAX);
    climit_set_usb(lim);
    nconfig_read(USB_CRITICAL_CURRENT_LIMIT, buf, sizeof(buf));
    lim = clamp_critical_current_limit(atof(buf), USB_CRITICAL_CURRENT_LIMIT_MAX);
    climit_set_critical_usb(lim);

    const esp_timer_create_args_t sensor_timer_args = {.callback = &sensor_timer_callback,
                                                       .name = "sensor_reading_timer"};
    const esp_timer_create_args_t wifi_timer_args = {.callback = &status_wifi_callback, .name = "wifi_status_timer"};
    const esp_timer_create_args_t long_press_timer_args = {.callback = &long_press_timer_callback,
                                                           .name = "long_press_timer"};

    ESP_ERROR_CHECK(esp_timer_create(&sensor_timer_args, &sensor_timer));
    ESP_ERROR_CHECK(esp_timer_create(&wifi_timer_args, &wifi_status_timer));
    ESP_ERROR_CHECK(esp_timer_create(&long_press_timer_args, &long_press_timer));

    xTaskCreate(shutdown_load_sw_task, "shutdown_sw_task", configMINIMAL_STACK_SIZE * 3, NULL, 15,
                &shutdown_task_handle);
    xTaskCreate(warning_alert_task, "warning_alert_task", configMINIMAL_STACK_SIZE * 3, NULL, 10,
                &warning_task_handle);
    if ((critical_cutoff_pending || gpio_get_level(PM_INT_CRITICAL) == 0) && shutdown_task_handle != NULL)
    {
        critical_cutoff_pending = false;
        gpio_set_level(PM_EXPANDER_RST, 0);
        xTaskNotifyGive(shutdown_task_handle);
    }
    else
    {
        // The limit setup above read the Mask/Enable register, which parks any critical
        // flag latched before this boot. No cut-off is pending, so drop it; left there it
        // would be blamed for the next button press.
        take_pending_flags(&pending_critical_flags);
    }
    if (gpio_get_level(PM_INT_WARNING) == 0 && warning_task_handle != NULL)
        xTaskNotifyGive(warning_task_handle);

    // Started after the boot checks so that its reads cannot park a flag that the check
    // above then drops. Above httpd (12), so serving a page does not make it miss
    // conversion cycles; it holds the CPU for well under a millisecond per cycle. Measured
    // use is ~820 bytes on the normal path; the rest is for I2C error logging.
    xTaskCreate(sampler_task, "ina3221_sampler", 2048, NULL, 13, NULL);

    nconfig_read(SENSOR_PERIOD_MS, buf, sizeof(buf));
    ESP_ERROR_CHECK(esp_timer_start_periodic(sensor_timer, strtol(buf, NULL, 10) * 1000));
    ESP_ERROR_CHECK(esp_timer_start_periodic(wifi_status_timer, 1000000 * 5));
}

esp_err_t update_sensor_period(int period)
{
    if (period < 100 || period > 10000) // 0.1 sec ~ 10 sec
    {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[10];
    sprintf(buf, "%d", period);
    esp_err_t err = nconfig_write(SENSOR_PERIOD_MS, buf);
    if (err != ESP_OK) {
        return err;
    }

    esp_timer_stop(sensor_timer);
    return esp_timer_start_periodic(sensor_timer, period * 1000);
}
