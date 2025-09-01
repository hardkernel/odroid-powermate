//
// Created by shinys on 25. 9. 1.
//

#include "priv_wifi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nconfig.h"

#include "wifi.h"

#include "indicator.h"

static const char* TAG = "WIFI";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
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
        led_set(LED_RED, BLINK_TRIPLE);
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason: %s", wifi_reason_str(event->reason));
        // ESP-IDF will automatically try to reconnect by default.
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        led_set(LED_BLU, BLINK_SOLID);
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        sync_time();
    }
}

/**
 * @brief Initializes Wi-Fi, starts in APSTA mode.
 * This function should be called once from the application's main function.
 */
void wifi_init(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    initialize_sntp();

    // Set default mode to APSTA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Initialize and configure AP and STA parts
    wifi_init_ap();
    wifi_init_sta();

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    led_set(LED_BLU, BLINK_TRIPLE);
    ESP_LOGI(TAG, "wifi_init_all finished. Started in APSTA mode.");
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

    wifi_mode_t current_mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&current_mode));
    if (current_mode == new_mode)
    {
        ESP_LOGI(TAG, "Already in %s mode", mode);
        return ESP_OK;
    }

    nconfig_write(WIFI_MODE, mode);

    // To change mode, we need to stop wifi, set mode, and start again.
    // This will cause a temporary disconnection but does not reboot the device.
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(new_mode));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi mode switched to %s", mode);

    return ESP_OK;
}
