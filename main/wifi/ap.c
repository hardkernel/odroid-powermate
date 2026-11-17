/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 25. 9. 1.
//

#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nconfig.h"
#include "priv_wifi.h"
#include "wifi.h"

static const char* TAG = "AP";

#define AP_CHANNEL 1
#define AP_MAX_CONN 4

// What the Wi-Fi driver takes for a saved password: none (open), 8-63 bytes of any value,
// or 64 hex digits. New passwords are held to printable ASCII (wifi_ap_config_valid());
// one saved by an older firmware that the driver accepts keeps working.
static bool driver_accepts_ap_password(const char* password)
{
    size_t len = strlen(password);
    if (len == 64)
    {
        for (const char* p = password; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
                return false;
        return true;
    }
    return len == 0 || (len >= 8 && len <= 63);
}

static esp_err_t set_ap_config(const char* ssid, const char* password)
{
    wifi_config_t wifi_config = {
        .ap =
            {
                .channel = AP_CHANNEL,
                .max_connection = AP_MAX_CONN,
                .authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
                .pmf_cfg =
                    {
                        .required = false,
                    },
            },
    };
    // Checked by the callers: at most 32 and 64 bytes; the fields need no NUL at full length.
    memcpy(wifi_config.ap.ssid, ssid, strlen(ssid));
    wifi_config.ap.ssid_len = strlen(ssid);
    memcpy(wifi_config.ap.password, password, strlen(password));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    memset(&wifi_config, 0, sizeof(wifi_config));
    return err;
}

/**
 * @brief Initializes and configures the AP mode.
 *
 * An empty saved password is an open AP. A saved SSID or password the Wi-Fi driver would
 * refuse (left by an older firmware, or unreadable) falls back to the factory WPA2 setting
 * instead of stopping the boot, and never to an open AP.
 */
esp_err_t wifi_init_ap(void)
{
    // Get the network interface handle for the AP
    esp_netif_t* p_netif_ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (p_netif_ap)
    {
        ESP_LOGI(TAG, "Setting AP static IP to 192.168.4.1");
        esp_netif_dhcps_stop(p_netif_ap); // Stop DHCP server to apply new IP settings

        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(p_netif_ap, &ip_info);

        esp_netif_dhcps_start(p_netif_ap); // Restart DHCP server
    }

    // One byte more than the config fields, for the NUL NVS stores.
    char ssid[32 + 1] = "";
    char password[64 + 1] = "";
    if (nconfig_read(AP_SSID, ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0')
        strcpy(ssid, NCONFIG_DEFAULT_AP_SSID);
    if (nconfig_read(AP_PASSWORD, password, sizeof(password)) != ESP_OK)
    {
        ESP_LOGW(TAG, "Saved AP password unreadable; using the factory password");
        strcpy(password, NCONFIG_DEFAULT_AP_PASSWORD);
    }
    else if (!driver_accepts_ap_password(password))
    {
        ESP_LOGW(TAG, "Saved AP password has %u bytes, which WPA2 refuses; using the factory password",
                 (unsigned)strlen(password));
        strcpy(password, NCONFIG_DEFAULT_AP_PASSWORD);
    }

    bool open = password[0] == '\0';
    esp_err_t err = set_ap_config(ssid, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set the AP config (%s); using the factory SSID and password", esp_err_to_name(err));
        strcpy(ssid, NCONFIG_DEFAULT_AP_SSID);
        open = false;
        err = set_ap_config(NCONFIG_DEFAULT_AP_SSID, NCONFIG_DEFAULT_AP_PASSWORD);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "Failed to set the factory AP config: %s", esp_err_to_name(err));
    }

    if (err == ESP_OK)
        ESP_LOGI(TAG, "wifi_init_ap finished. SSID: %s, %s, Channel: %d", ssid, open ? "open" : "WPA2", AP_CHANNEL);
    return err;
}
