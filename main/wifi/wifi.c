//
// Created by shinys on 25. 9. 1.
//

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nconfig.h"
#include "priv_wifi.h"

#include "wifi.h"

#include "indicator.h"
static bool s_auto_reconnect = true;

static const char* TAG = "WIFI";

void wifi_set_auto_reconnect(bool enable) { s_auto_reconnect = enable; }


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
            esp_wifi_connect();
        }
        else
        {
            ESP_LOGI(TAG, "STA SSID not configured, not connecting.");
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        led_set(LED_BLU, BLINK_TRIPLE);
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason: %s", wifi_reason_str(event->reason));

        if (event->reason != WIFI_REASON_ASSOC_LEAVE)
        {
            if (s_auto_reconnect && !nconfig_value_is_not_set(WIFI_SSID))
            {
                ESP_LOGI(TAG, "Connection lost, attempting to reconnect...");
                esp_wifi_connect();
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
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

    nconfig_write(WIFI_MODE, mode);

    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(new_mode));

    if (new_mode == WIFI_MODE_APSTA)
    {
        wifi_init_ap();
        wifi_init_sta();
    }
    else if (new_mode == WIFI_MODE_STA)
    {
        wifi_init_sta();
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi mode switched to %s", mode);

    return ESP_OK;
}
