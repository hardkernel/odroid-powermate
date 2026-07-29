//
// Created by shinys on 25. 9. 1.
//

#include <assert.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nconfig.h"
#include "priv_wifi.h"

#include "wifi.h"

#include "indicator.h"
static bool s_auto_reconnect = true;
static EventGroupHandle_t s_sta_event_group;
static SemaphoreHandle_t s_wifi_control_mutex;
static TaskHandle_t s_reconnect_task;
static volatile bool s_sta_connect_requested;
static volatile bool s_sta_connected;
static volatile bool s_sta_credentials_pending_validation;
static volatile wifi_sta_connection_state_t s_sta_connection_state = WIFI_STA_CONNECTION_IDLE;
static volatile wifi_err_reason_t s_sta_connection_failure_reason = WIFI_REASON_UNSPECIFIED;
static volatile uint32_t s_reconnect_delay_ms = 1000;

#define STA_DISCONNECTED_BIT BIT0
#define RECONNECT_DELAY_MIN_MS 1000
#define RECONNECT_DELAY_MAX_MS 30000

static const char* TAG = "WIFI";

void wifi_set_auto_reconnect(bool enable) { s_auto_reconnect = enable; }
bool wifi_get_auto_reconnect(void) { return s_auto_reconnect; }
bool wifi_sta_is_connected(void) { return s_sta_connected; }
wifi_sta_connection_state_t wifi_get_sta_connection_state(void) { return s_sta_connection_state; }
wifi_err_reason_t wifi_get_sta_connection_failure_reason(void) { return s_sta_connection_failure_reason; }

static void wifi_set_sta_connection_state(wifi_sta_connection_state_t state, wifi_err_reason_t reason)
{
    s_sta_connection_state = state;
    s_sta_connection_failure_reason = reason;
}

void wifi_control_lock(void)
{
    assert(s_wifi_control_mutex != NULL);
    xSemaphoreTake(s_wifi_control_mutex, portMAX_DELAY);
}

void wifi_control_unlock(void)
{
    xSemaphoreGive(s_wifi_control_mutex);
}

void wifi_reset_reconnect_backoff(void)
{
    s_reconnect_delay_ms = RECONNECT_DELAY_MIN_MS;
}

esp_err_t wifi_connect_locked(void)
{
    if (s_sta_connect_requested)
        return ESP_OK;

    ESP_LOGI(TAG, "Connecting to AP...");
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK)
    {
        s_sta_connect_requested = true;
        wifi_set_sta_connection_state(WIFI_STA_CONNECTION_CONNECTING, WIFI_REASON_UNSPECIFIED);
    }
    return err;
}

void wifi_cancel_sta_connection(void)
{
    s_sta_connect_requested = false;
    s_sta_connected = false;
    wifi_set_sta_connection_state(WIFI_STA_CONNECTION_IDLE, WIFI_REASON_UNSPECIFIED);
}

void wifi_mark_sta_credentials_pending_validation(void)
{
    s_sta_credentials_pending_validation = true;
}

void wifi_schedule_reconnect(void)
{
    if (s_reconnect_task)
        xTaskNotifyGive(s_reconnect_task);
}

static bool reconnect_is_configuration_error(wifi_err_reason_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_GROUP_CIPHER_INVALID:
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
    case WIFI_REASON_AKMP_INVALID:
    case WIFI_REASON_INVALID_RSN_IE_CAP:
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return true;
    default:
        return false;
    }
}

static void wifi_reconnect_task(void* arg)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t delay_ms = s_reconnect_delay_ms;
        if (s_reconnect_delay_ms < RECONNECT_DELAY_MAX_MS / 2)
            s_reconnect_delay_ms *= 2;
        else
            s_reconnect_delay_ms = RECONNECT_DELAY_MAX_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        wifi_control_lock();
        wifi_mode_t mode;
        bool sta_mode_enabled =
            esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
        if (s_auto_reconnect && !nconfig_value_is_not_set(WIFI_SSID) && !s_sta_connect_requested &&
            s_sta_connection_state != WIFI_STA_CONNECTION_FAILED && sta_mode_enabled)
        {
            esp_err_t err = wifi_connect_locked();
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Reconnect request failed: %s", esp_err_to_name(err));
                wifi_schedule_reconnect();
            }
        }
        wifi_control_unlock();
    }
}

void wifi_prepare_sta_disconnect(void)
{
    if (s_sta_event_group)
        xEventGroupClearBits(s_sta_event_group, STA_DISCONNECTED_BIT);
}

bool wifi_wait_for_sta_disconnect(uint32_t timeout_ms)
{
    if (!s_sta_event_group)
        return false;

    EventBits_t bits = xEventGroupWaitBits(s_sta_event_group, STA_DISCONNECTED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & STA_DISCONNECTED_BIT) != 0;
}


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Station mode started");
        // Only try to connect if SSID is configured
        if (!nconfig_value_is_not_set(WIFI_SSID))
        {
            wifi_reset_reconnect_backoff();
            wifi_schedule_reconnect();
        }
        else
        {
            ESP_LOGI(TAG, "STA SSID not configured, not connecting.");
            wifi_set_sta_connection_state(WIFI_STA_CONNECTION_IDLE, WIFI_REASON_UNSPECIFIED);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        led_set(LED_BLU, BLINK_TRIPLE);
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
        s_sta_connect_requested = false;
        s_sta_connected = false;
        if (s_sta_event_group)
            xEventGroupSetBits(s_sta_event_group, STA_DISCONNECTED_BIT);
        ESP_LOGW(TAG, "Disconnected from AP, reason: %s (%u)", wifi_reason_str(event->reason), event->reason);

        // Only reject credentials while validating a newly submitted configuration.
        // A previously working connection must keep retrying after transient RF errors.
        bool configuration_error =
            s_sta_credentials_pending_validation && reconnect_is_configuration_error(event->reason);
        if (configuration_error)
        {
            wifi_set_sta_connection_state(WIFI_STA_CONNECTION_FAILED, event->reason);
        }
        else if (event->reason != WIFI_REASON_ASSOC_LEAVE && event->reason != WIFI_REASON_STA_LEAVING)
        {
            if (s_auto_reconnect && !nconfig_value_is_not_set(WIFI_SSID))
            {
                wifi_set_sta_connection_state(WIFI_STA_CONNECTION_CONNECTING, WIFI_REASON_UNSPECIFIED);
                ESP_LOGI(TAG, "Connection lost, scheduling reconnect");
                wifi_schedule_reconnect();
            }
            else
            {
                wifi_set_sta_connection_state(WIFI_STA_CONNECTION_FAILED, event->reason);
            }
        }

        if (configuration_error)
        {
            ESP_LOGW(TAG, "Reconnect paused until Wi-Fi credentials are changed");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_sta_connect_requested = true;
        s_sta_connected = true;
        s_sta_credentials_pending_validation = false;
        wifi_set_sta_connection_state(WIFI_STA_CONNECTION_CONNECTED, WIFI_REASON_UNSPECIFIED);
        wifi_reset_reconnect_backoff();
        led_set(LED_BLU, BLINK_SOLID);
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        sync_time();
    }
}

void wifi_init(void)
{
    // Create network interfaces for both AP and STA.
    // This is done unconditionally to allow for dynamic mode switching.
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_sta_event_group = xEventGroupCreate();
    assert(s_sta_event_group != NULL);
    s_wifi_control_mutex = xSemaphoreCreateMutex();
    assert(s_wifi_control_mutex != NULL);
    BaseType_t task_created = xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 3072, NULL, 5, &s_reconnect_task);
    assert(task_created == pdPASS);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    initialize_sntp();

    char mode_str[10] = {0};
    wifi_mode_t mode = WIFI_MODE_APSTA;
    const char* started_mode_str = "APSTA";

    if (nconfig_read(WIFI_MODE, mode_str, sizeof(mode_str)) == ESP_OK)
    {
        if (strcmp(mode_str, "sta") == 0)
        {
            mode = WIFI_MODE_STA;
            started_mode_str = "STA";
        }
        else if (strcmp(mode_str, "apsta") != 0)
        {
            ESP_LOGW(TAG, "Invalid Wi-Fi mode in nconfig: '%s'. Defaulting to APSTA.", mode_str);
        }
    }
    else
    {
        ESP_LOGW(TAG, "Failed to read Wi-Fi mode from nconfig. Defaulting to APSTA.");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (mode == WIFI_MODE_APSTA)
    {
        wifi_init_ap();
        wifi_init_sta();
    }
    else if (mode == WIFI_MODE_STA)
    {
        wifi_init_sta();
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    led_set(LED_BLU, BLINK_TRIPLE);
    ESP_LOGI(TAG, "wifi_init_all finished. Started in %s mode.", started_mode_str);
}

esp_err_t wifi_switch_mode(const char* mode)
{
    ESP_LOGI(TAG, "Switching Wi-Fi mode to %s", mode);

    wifi_mode_t new_mode;
    if (strcmp(mode, "sta") == 0)
    {
        new_mode = WIFI_MODE_STA;
    }
    else if (strcmp(mode, "apsta") == 0)
    {
        new_mode = WIFI_MODE_APSTA;
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported mode: %s", mode);
        return ESP_ERR_INVALID_ARG;
    }

    wifi_control_lock();
    bool auto_reconnect = wifi_get_auto_reconnect();
    wifi_set_auto_reconnect(false);
    wifi_cancel_sta_connection();

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to stop Wi-Fi: %s", esp_err_to_name(err));
        goto out;
    }

    err = esp_wifi_set_mode(new_mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set Wi-Fi mode: %s", esp_err_to_name(err));
        goto out;
    }

    if (new_mode == WIFI_MODE_APSTA)
    {
        wifi_init_ap();
        wifi_init_sta();
    }
    else if (new_mode == WIFI_MODE_STA)
    {
        wifi_init_sta();
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
        goto out;
    }

    err = nconfig_write(WIFI_MODE, mode);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to save Wi-Fi mode: %s", esp_err_to_name(err));

out:
    wifi_set_auto_reconnect(auto_reconnect);
    if (err == ESP_OK && auto_reconnect && !nconfig_value_is_not_set(WIFI_SSID))
    {
        wifi_reset_reconnect_backoff();
        wifi_schedule_reconnect();
    }
    wifi_control_unlock();

    if (err == ESP_OK)
        ESP_LOGI(TAG, "Wi-Fi mode switched to %s", mode);

    return err;
}
