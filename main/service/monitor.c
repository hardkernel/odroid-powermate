//
// Created by shinys on 25. 8. 18..
//

#include "monitor.h"
#include <nconfig.h>
#include <sys/time.h>
#include <time.h>
#include "climit.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" // Added for FreeRTOS tasks
#include "ina3221.h"
#include "pb.h"
#include "pb_encode.h"
#include "status.pb.h"
#include "sw.h"
#include "webserver.h"
#include "wifi.h"

#define CHANNEL_VIN INA3221_CHANNEL_3
#define CHANNEL_MAIN INA3221_CHANNEL_2
#define CHANNEL_USB INA3221_CHANNEL_1

#define PM_SDA CONFIG_I2C_GPIO_SDA
#define PM_SCL CONFIG_I2C_GPIO_SCL

#define PM_INT_CRITICAL CONFIG_GPIO_INA3221_INT_CRITICAL
#define PM_EXPANDER_RST CONFIG_GPIO_EXPANDER_RESET

#define PB_BUFFER_SIZE 256

static const char* TAG = "monitor";

static esp_timer_handle_t sensor_timer;
static esp_timer_handle_t wifi_status_timer;
static esp_timer_handle_t long_press_timer;
// static esp_timer_handle_t shutdown_load_sw; // No longer needed

static TaskHandle_t shutdown_task_handle = NULL; // Global task handle

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
            .avg = INA3221_AVG_64, // 64 samples average
            .vbus = INA3221_CT_2116, // 2ms by channel (bus)
            .vsht = INA3221_CT_2116, // 2ms by channel (shunt)
        },
};

static bool encode_string(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
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

static void send_pb_message(const pb_msgdesc_t* fields, const void* src_struct)
{
    uint8_t buffer[PB_BUFFER_SIZE];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, fields, src_struct))
    {
        ESP_LOGE(TAG, "Failed to encode protobuf message: %s", PB_GET_ERROR(&stream));
        return;
    }

    push_data_to_ws(buffer, stream.bytes_written);
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
        gpio_set_level(PM_EXPANDER_RST, 0);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_set_level(PM_EXPANDER_RST, 1);
        config_sw();

        // Start a 5-second timer to check for long press
        esp_timer_start_once(long_press_timer, 5000000);
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

static void gpio_init()
{
    // critical int
    gpio_set_intr_type(PM_INT_CRITICAL, GPIO_INTR_ANYEDGE);
    gpio_set_direction(PM_INT_CRITICAL, GPIO_MODE_INPUT);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PM_INT_CRITICAL, critical_isr_handler, (void*)PM_INT_CRITICAL);

    // rst expander
    gpio_set_level(PM_EXPANDER_RST, 1);
    gpio_set_direction(PM_EXPANDER_RST, GPIO_MODE_OUTPUT);
}

esp_err_t climit_set_vin(double value)
{
    float lim = (float)(value * 1000);
    ESP_LOGI(TAG, "Setting VIN current limit to: %fmA", lim);
    if (value > 0.0f)
        return ina3221_set_critical_alert(&ina3221, CHANNEL_VIN, lim);
    return ina3221_set_critical_alert(&ina3221, CHANNEL_VIN, (15.0f * 1000.0f));
}

esp_err_t climit_set_main(double value)
{
    float lim = (float)(value * 1000);
    ESP_LOGI(TAG, "Setting MAIN current limit to: %fmA", lim);
    if (value > 0.0f)
        return ina3221_set_critical_alert(&ina3221, CHANNEL_MAIN, lim);
    return ina3221_set_critical_alert(&ina3221, CHANNEL_VIN, (15.0f * 1000.0f));
}

esp_err_t climit_set_usb(double value)
{
    float lim = (float)(value * 1000);
    ESP_LOGI(TAG, "Setting USB current limit to: %fmA", lim);
    if (value > 0.0f)
        return ina3221_set_critical_alert(&ina3221, CHANNEL_USB, lim);
    return ina3221_set_critical_alert(&ina3221, CHANNEL_VIN, (15.0f * 1000.0f));
}

void init_status_monitor()
{
    gpio_init();
    ESP_ERROR_CHECK(ina3221_init_desc(&ina3221, 0x40, 0, PM_SDA, PM_SCL));

    double lim;
    char buf[10];

    nconfig_read(VIN_CURRENT_LIMIT, buf, sizeof(buf));
    lim = atof(buf);
    climit_set_vin(lim);

    nconfig_read(MAIN_CURRENT_LIMIT, buf, sizeof(buf));
    lim = atof(buf);
    climit_set_main(lim);

    nconfig_read(USB_CURRENT_LIMIT, buf, sizeof(buf));
    lim = atof(buf);
    climit_set_usb(lim);

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

    nconfig_read(SENSOR_PERIOD_MS, buf, sizeof(buf));
    ESP_ERROR_CHECK(esp_timer_start_periodic(sensor_timer, strtol(buf, NULL, 10) * 1000));
    ESP_ERROR_CHECK(esp_timer_start_periodic(wifi_status_timer, 1000000 * 5));
}

esp_err_t update_sensor_period(int period)
{
    if (period < 500 || period > 10000) // 0.5 sec ~ 10 sec
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