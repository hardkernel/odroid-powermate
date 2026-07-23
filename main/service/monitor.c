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
#include "freertos/task.h" // Added for FreeRTOS tasks
#include "ina3221.h"
#include "pbmsg.h"
#include "sw.h"
#include "webserver.h"
#include "wifi.h"

#define CHANNEL_VIN INA3221_CHANNEL_3
#define CHANNEL_MAIN INA3221_CHANNEL_2
#define CHANNEL_USB INA3221_CHANNEL_1

#define PM_SDA CONFIG_I2C_GPIO_SDA
#define PM_SCL CONFIG_I2C_GPIO_SCL

#define PM_INT_CRITICAL CONFIG_GPIO_INA3221_INT_CRITICAL
#define PM_INT_WARNING CONFIG_GPIO_INA3221_INT_WARNING
#define PM_EXPANDER_RST CONFIG_GPIO_EXPANDER_RESET

#define INA3221_REG_CRITICAL_ALERT_1 0x07
#define INA3221_REG_WARNING_ALERT_1 0x08
#define INA3221_REG_MASK 0x0F
#define CLIMIT_DISABLED_LIMIT_A 15.0f
#define CLIMIT_VERIFY_TOLERANCE_A 0.01f

static const char* TAG = "monitor";

static esp_timer_handle_t sensor_timer;
static esp_timer_handle_t wifi_status_timer;
static esp_timer_handle_t long_press_timer;
// static esp_timer_handle_t shutdown_load_sw; // No longer needed

static TaskHandle_t shutdown_task_handle = NULL; // Global task handle
static TaskHandle_t warning_task_handle = NULL;
static volatile uint16_t pending_critical_flags = 0;
static volatile uint16_t pending_warning_flags = 0;
static uint16_t last_warning_flags = 0;

ina3221_t ina3221 = {
    .shunt = {10, 10, 10},
    .mask.mask_register = INA3221_DEFAULT_MASK,
    .i2c_dev = {0},
    .config =
        {
            .mode = true, // mode selection
            .esht = true, // shunt enable
            .ebus = true, // bus enable
            .ch1 = true, // channel 1 enable
            .ch2 = true, // channel 2 enable
            .ch3 = true, // channel 3 enable
            .avg = INA3221_AVG_16, // 16 samples average
            .vbus = INA3221_CT_140, // 140us by channel (bus)
            .vsht = INA3221_CT_1100, // 1.1ms by channel (shunt)
        },
};

typedef struct
{
    const char* name;
    ina3221_channel_t channel;
    uint16_t cf_bit;
} monitor_channel_t;

static const monitor_channel_t monitor_channels[] = {
    {.name = "USB", .channel = CHANNEL_USB, .cf_bit = BIT2},   // IN1
    {.name = "MAIN", .channel = CHANNEL_MAIN, .cf_bit = BIT1}, // IN2
    {.name = "VIN", .channel = CHANNEL_VIN, .cf_bit = BIT0},   // IN3
};

static esp_err_t ina3221_read_reg16(uint8_t reg, uint16_t* val)
{
    if (!val)
        return ESP_ERR_INVALID_ARG;

    uint16_t raw;
    esp_err_t err = i2c_dev_take_mutex(&ina3221.i2c_dev);
    if (err != ESP_OK)
        return err;

    err = i2c_dev_read_reg(&ina3221.i2c_dev, reg, &raw, sizeof(raw));
    esp_err_t unlock_err = i2c_dev_give_mutex(&ina3221.i2c_dev);

    if (err != ESP_OK)
        return err;
    if (unlock_err != ESP_OK)
        return unlock_err;

    *val = (raw >> 8) | (raw << 8);
    return ESP_OK;
}

static void notify_alert_tasks(uint16_t cf, uint16_t wf)
{
    if (cf)
    {
        pending_critical_flags |= cf;
        if (shutdown_task_handle != NULL)
            xTaskNotifyGive(shutdown_task_handle);
    }

    if (wf)
    {
        pending_warning_flags |= wf;
        if (warning_task_handle != NULL)
            xTaskNotifyGive(warning_task_handle);
    }
}

static float climit_raw_to_a(ina3221_channel_t channel, uint16_t raw)
{
    int16_t signed_raw = (int16_t)raw;
    float current_ma = (float)signed_raw / ((float)ina3221.shunt[channel] * 0.2f);
    return current_ma / 1000.0f;
}

static esp_err_t climit_get_channel_reg(ina3221_channel_t channel, uint8_t base_reg, float* limit_a, uint16_t* raw)
{
    uint16_t raw_value;
    esp_err_t err = ina3221_read_reg16(base_reg + channel * 2, &raw_value);
    if (err != ESP_OK)
        return err;

    if (raw)
        *raw = raw_value;
    if (limit_a)
        *limit_a = climit_raw_to_a(channel, raw_value);

    return ESP_OK;
}

static esp_err_t climit_get_channel(ina3221_channel_t channel, float* limit_a, uint16_t* raw)
{
    return climit_get_channel_reg(channel, INA3221_REG_WARNING_ALERT_1, limit_a, raw);
}

static esp_err_t climit_get_critical_channel(ina3221_channel_t channel, float* limit_a, uint16_t* raw)
{
    return climit_get_channel_reg(channel, INA3221_REG_CRITICAL_ALERT_1, limit_a, raw);
}

static esp_err_t climit_set_channel(const char* name, const char* label, ina3221_channel_t channel, double value,
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

    esp_err_t err = is_critical ? ina3221_set_critical_alert(&ina3221, channel, applied_limit_ma)
                                : ina3221_set_warning_alert(&ina3221, channel, applied_limit_ma);
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
    esp_err_t mask_err = ina3221_read_reg16(INA3221_REG_MASK, &mask_raw);
    if (mask_err == ESP_OK)
    {
        ESP_LOGI(TAG, "%s %s status: critical_gpio=%d warning_gpio=%d mask=0x%04x", name, label,
                 gpio_get_level(PM_INT_CRITICAL), gpio_get_level(PM_INT_WARNING), mask_raw);
    }

    return ESP_OK;
}

static esp_err_t read_channel_snapshot(const monitor_channel_t* channel, float* voltage, float* current_a)
{
    float current_ma;
    esp_err_t err = ina3221_get_bus_voltage(&ina3221, channel->channel, voltage);
    if (err != ESP_OK)
        return err;

    err = ina3221_get_shunt_value(&ina3221, channel->channel, NULL, &current_ma);
    if (err != ESP_OK)
        return err;

    *current_a = current_ma / 1000.0f;
    return ESP_OK;
}

static void push_critical_fault_details(uint16_t cf)
{
    ESP_LOGW(TAG, "critical fault summary: cf=0x%x int_gpio=%d", cf, gpio_get_level(PM_INT_CRITICAL));

    bool any_channel_fault = false;
    for (size_t i = 0; i < sizeof(monitor_channels) / sizeof(monitor_channels[0]); ++i)
    {
        const monitor_channel_t* channel = &monitor_channels[i];
        bool flagged = (cf & channel->cf_bit) != 0;
        any_channel_fault = any_channel_fault || flagged;

        float voltage = 0.0f;
        float current_a = 0.0f;
        float limit_a = 0.0f;
        uint16_t raw_limit = 0;
        esp_err_t sample_err = read_channel_snapshot(channel, &voltage, &current_a);
        esp_err_t limit_err = climit_get_critical_channel(channel->channel, &limit_a, &raw_limit);

        if (sample_err == ESP_OK && limit_err == ESP_OK)
        {
            ESP_LOGW(TAG, "critical detail: %s flagged=%d current=%.3fA voltage=%.3fV limit=%.3fA raw=0x%04x",
                     channel->name, flagged, current_a, voltage, limit_a, raw_limit);
            push_eventf(EV_CRITICAL, "critical detail: %s current=%.3fA voltage=%.3fV limit=%.3fA", channel->name,
                        current_a, voltage, limit_a);
        }
        else
        {
            ESP_LOGW(TAG, "critical detail: %s read failed: sample=%s, limit=%s", channel->name,
                     esp_err_to_name(sample_err), esp_err_to_name(limit_err));
            push_eventf(EV_CRITICAL, "critical detail: %s read failed: sample=%s, limit=%s", channel->name,
                        esp_err_to_name(sample_err), esp_err_to_name(limit_err));
        }
    }

    if (!any_channel_fault)
    {
        ESP_LOGW(TAG, "critical input asserted without INA3221 channel flag");
        push_eventf(EV_CRITICAL, "critical input asserted without INA3221 channel flag");
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

static uint16_t take_pending_flags(volatile uint16_t* flags)
{
    uint16_t value = *flags;
    *flags = 0;
    return value;
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

    for (uint8_t i = 0; i < INA3221_BUS_NUMBER; i++)
    {
        float voltage, current, power;
        ina3221_get_bus_voltage(&ina3221, i, &voltage);
        ina3221_get_shunt_value(&ina3221, i, NULL, &current);

        current /= 1000.0f; // mA to A
        power = voltage * current;

        // For protobuf
        channels[i]->voltage = voltage;
        channels[i]->current = current;
        channels[i]->power = power;
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

        ESP_LOGW(TAG, "critical interrupt triggered (via task)");
        uint16_t cf = take_pending_flags(&pending_critical_flags);
        esp_err_t status_err = ina3221_get_status(&ina3221);

        if (status_err == ESP_OK)
        {
            cf |= ina3221.mask.cf; // BIT2=IN1/USB, BIT1=IN2/MAIN, BIT0=IN3/VIN
            notify_alert_tasks(0, ina3221.mask.wf);
            push_critical_fault_details(cf);
        }
        else
        {
            ESP_LOGW(TAG, "failed to read INA3221 status on critical interrupt: %s", esp_err_to_name(status_err));
            push_eventf(EV_CRITICAL, "critical fault: INA3221 status read failed: %s", esp_err_to_name(status_err));
        }

        gpio_set_level(PM_EXPANDER_RST, 0);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_set_level(PM_EXPANDER_RST, 1);
        config_sw();
        esp_err_t persist_err = persist_load_switch_state();
        if (persist_err != ESP_OK)
            ESP_LOGW(TAG, "failed to save disabled load switch state: %s", esp_err_to_name(persist_err));

        push_eventf(EV_CRITICAL, "load switch disabled");

        if (cf & BIT0) // CH3 VIN
            push_eventf(EV_CRITICAL, "critical fault detected: VIN");
        else if (cf & BIT1) // CH2 VOUT
            push_eventf(EV_CRITICAL, "critical fault detected: MAIN");
        else if (cf & BIT2) // CH1 USB
            push_eventf(EV_CRITICAL, "critical fault detected: USB");
        else
            push_eventf(EV_CRITICAL, "critical fault detected: BTN");

        // Start a 5-second timer to check for long press
        esp_timer_start_once(long_press_timer, 5000000);
    }
}

static void warning_alert_task(void* pvParameters)
{
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG, "warning interrupt triggered (via task)");
        uint16_t wf = take_pending_flags(&pending_warning_flags);
        esp_err_t status_err = ina3221_get_status(&ina3221);

        if (status_err == ESP_OK)
        {
            wf |= ina3221.mask.wf;
            notify_alert_tasks(ina3221.mask.cf, 0);
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
        if (shutdown_task_handle != NULL)
        {
            vTaskNotifyGiveFromISR(shutdown_task_handle, &xHigherPriorityTaskWoken);
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
    // critical int
    gpio_set_intr_type(PM_INT_CRITICAL, GPIO_INTR_ANYEDGE);
    gpio_set_direction(PM_INT_CRITICAL, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PM_INT_CRITICAL, GPIO_PULLUP_ONLY);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PM_INT_CRITICAL, critical_isr_handler, (void*)PM_INT_CRITICAL);

    // warning int
    gpio_set_intr_type(PM_INT_WARNING, GPIO_INTR_ANYEDGE);
    gpio_set_direction(PM_INT_WARNING, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PM_INT_WARNING, GPIO_PULLUP_ONLY);
    gpio_isr_handler_add(PM_INT_WARNING, warning_isr_handler, (void*)PM_INT_WARNING);

    // rst expander
    gpio_set_level(PM_EXPANDER_RST, 1);
    gpio_set_direction(PM_EXPANDER_RST, GPIO_MODE_OUTPUT);
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
    gpio_init();
    ESP_ERROR_CHECK(ina3221_init_desc(&ina3221, 0x40, 0, PM_SDA, PM_SCL));
    ESP_ERROR_CHECK(ina3221_sync(&ina3221));

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
    if (gpio_get_level(PM_INT_WARNING) == 0 && warning_task_handle != NULL)
        xTaskNotifyGive(warning_task_handle);

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
