//
// Created by shinys on 25. 9. 1.
//

#include "priv_wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "nconfig.h"
#include "esp_netif.h"
#include "wifi.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "AP";

#define DEFAULT_AP_SSID "odroid-pm"
#define DEFAULT_AP_PASS "powermate"
#define AP_CHANNEL 1
#define AP_MAX_CONN 4

/**
 * @brief Initializes and configures the AP mode.
 */
void wifi_init_ap(void)
{
    // Get the network interface handle for the AP
    esp_netif_t *p_netif_ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (p_netif_ap) {
        ESP_LOGI(TAG, "Setting AP static IP to 192.168.4.1");
        esp_netif_dhcps_stop(p_netif_ap); // Stop DHCP server to apply new IP settings

        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(p_netif_ap, &ip_info);

        esp_netif_dhcps_start(p_netif_ap); // Restart DHCP server
    }

    // Configure Wi-Fi AP settings
    wifi_config_t wifi_config = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                    .required = false,
            },
        },
    };

    // Read SSID and password from NVS (nconfig)
    size_t len;
    if (nconfig_get_str_len(AP_SSID, &len) == ESP_OK && len > 1) {
        nconfig_read(AP_SSID, (char*)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid));
    } else {
        strcpy((char*)wifi_config.ap.ssid, DEFAULT_AP_SSID);
    }

    if (nconfig_get_str_len(AP_PASSWORD, &len) == ESP_OK && len > 1) {
        nconfig_read(AP_PASSWORD, (char*)wifi_config.ap.password, sizeof(wifi_config.ap.password));
    } else {
        strcpy((char*)wifi_config.ap.password, DEFAULT_AP_PASS);
    }

    // If password is not set, use open authentication
    if (strlen((char*)wifi_config.ap.password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_LOGI(TAG, "wifi_init_ap finished. SSID: %s, Password: %s, Channel: %d",
             (char*)wifi_config.ap.ssid, "********", AP_CHANNEL);
}
