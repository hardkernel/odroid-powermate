//
// Created by shinys on 25. 9. 1.
//

#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nconfig.h"
#include "priv_wifi.h"
#include "wifi.h"

static const char* TAG = "STA";

#define WIFI_DISCONNECT_WAIT_MS 500
#define WIFI_CONFIG_RETRY_COUNT 10
#define WIFI_CONFIG_RETRY_DELAY_MS 100

static esp_err_t wifi_use_dhcp_locked(void);
static esp_err_t wifi_use_static_locked(const char* ip, const char* gw, const char* netmask,
                                        const char* dns1, const char* dns2);

/**
 * @brief Initializes and configures the STA mode.
 */
void wifi_init_sta(void)
{
    wifi_config_t wifi_config = {0};

    // Read SSID and password from NVS (nconfig)
    size_t len;
    if (nconfig_get_str_len(WIFI_SSID, &len) == ESP_OK && len > 1)
    {
        nconfig_read(WIFI_SSID, (char*)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid));
    }
    else
    {
        ESP_LOGW(TAG, "STA SSID not configured in NVS.");
    }

    if (nconfig_get_str_len(WIFI_PASSWORD, &len) == ESP_OK && len > 1)
    {
        nconfig_read(WIFI_PASSWORD, (char*)wifi_config.sta.password, sizeof(wifi_config.sta.password));
    }
    else
    {
        ESP_LOGW(TAG, "STA Password not configured in NVS.");
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Check if we should use a static IP
    char netif_type[16] = {0};
    if (nconfig_get_str_len(NETIF_TYPE, &len) == ESP_OK && len > 1)
    {
        nconfig_read(NETIF_TYPE, netif_type, sizeof(netif_type));
        if (strcmp(netif_type, "static") == 0)
        {
            ESP_LOGI(TAG, "Using static IP configuration for STA.");
            char ip[16], gw[16], netmask[16], dns1[16], dns2[16];
            nconfig_read(NETIF_IP, ip, sizeof(ip));
            nconfig_read(NETIF_GATEWAY, gw, sizeof(gw));
            nconfig_read(NETIF_SUBNET, netmask, sizeof(netmask));
            nconfig_read(NETIF_DNS1, dns1, sizeof(dns1));
            nconfig_read(NETIF_DNS2, dns2, sizeof(dns2));
            wifi_use_static_locked(ip, gw, netmask, dns1, dns2);
        }
    }

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

esp_err_t wifi_connect(void)
{
    wifi_control_lock();
    wifi_reset_reconnect_backoff();
    esp_err_t err = wifi_connect_locked();
    wifi_control_unlock();
    return err;
}

esp_err_t wifi_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from AP...");
    wifi_control_lock();
    wifi_cancel_sta_connection();
    wifi_prepare_sta_disconnect();
    esp_err_t err = esp_wifi_disconnect();
    wifi_control_unlock();
    return err;
}

void wifi_scan_aps(wifi_ap_record_t** ap_records, uint16_t* count)
{
    ESP_LOGI(TAG, "Scanning for APs...");
    *count = 0;
    *ap_records = NULL;

    wifi_control_lock();
    bool auto_reconnect = wifi_get_auto_reconnect();
    wifi_set_auto_reconnect(false);

    if (!wifi_sta_is_connected())
    {
        wifi_cancel_sta_connection();
        wifi_prepare_sta_disconnect();
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err == ESP_OK)
        {
            wifi_wait_for_sta_disconnect(WIFI_DISCONNECT_WAIT_MS);
        }
        else if (disconnect_err != ESP_ERR_WIFI_NOT_CONNECT)
        {
            ESP_LOGW(TAG, "Failed to stop STA connection before scan: %s", esp_err_to_name(disconnect_err));
        }
    }

    // Start scan, this is a blocking call. A connected STA is left associated.
    esp_err_t scan_err = esp_wifi_scan_start(NULL, true);
    if (scan_err == ESP_OK)
    {
        esp_wifi_scan_get_ap_num(count);
        ESP_LOGI(TAG, "Found %d APs", *count);
        if (*count > 0)
        {
            *ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * (*count));
            if (*ap_records != NULL)
            {
                esp_wifi_scan_get_ap_records(count, *ap_records);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to allocate memory for AP records");
                *count = 0;
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "Failed to scan for APs: %s", esp_err_to_name(scan_err));
    }

    wifi_set_auto_reconnect(auto_reconnect);

    bool reconnect_after_scan = false;
    if (!wifi_sta_is_connected())
    {
        if (!nconfig_value_is_not_set(WIFI_SSID))
            reconnect_after_scan = auto_reconnect;
    }
    wifi_control_unlock();

    if (reconnect_after_scan)
    {
        wifi_reset_reconnect_backoff();
        wifi_schedule_reconnect();
    }
}

esp_err_t wifi_get_current_ap_info(wifi_ap_record_t* ap_info)
{
    if (ap_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_sta_is_connected())
        return ESP_ERR_WIFI_NOT_CONNECT;
    return esp_wifi_sta_get_ap_info(ap_info);
}

esp_err_t wifi_get_current_ip_info(esp_netif_ip_info_t* ip_info)
{
    if (ip_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_sta_is_connected())
        return ESP_ERR_WIFI_NOT_CONNECT;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        return ESP_FAIL;
    }
    return esp_netif_get_ip_info(netif, ip_info);
}

esp_err_t wifi_get_dns_info(esp_netif_dns_type_t type, esp_netif_dns_info_t* dns_info)
{
    if (dns_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        return ESP_FAIL;
    }
    return esp_netif_get_dns_info(netif, type, dns_info);
}

esp_err_t wifi_use_dhcp(void)
{
    wifi_control_lock();
    esp_err_t err = wifi_use_dhcp_locked();
    wifi_control_unlock();
    return err;
}

static esp_err_t wifi_use_dhcp_locked(void)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Setting STA to use DHCP");
    esp_err_t err = esp_netif_dhcpc_start(netif);
    if (err == ESP_OK)
    {
        nconfig_write(NETIF_TYPE, "dhcp");
    }
    return err;
}

esp_err_t wifi_use_static(const char* ip, const char* gw, const char* netmask, const char* dns1, const char* dns2)
{
    wifi_control_lock();
    esp_err_t err = wifi_use_static_locked(ip, gw, netmask, dns1, dns2);
    wifi_control_unlock();
    return err;
}

static esp_err_t wifi_use_static_locked(const char* ip, const char* gw, const char* netmask, const char* dns1,
                                        const char* dns2)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Setting STA to use static IP");
    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGE(TAG, "Failed to stop DHCP client: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = ipaddr_addr(ip);
    ip_info.gw.addr = ipaddr_addr(gw);
    ip_info.netmask.addr = ipaddr_addr(netmask);

    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set static IP: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4.addr = ipaddr_addr(dns1);
    dns_info.ip.type = IPADDR_TYPE_V4;
    err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set main DNS: %s", esp_err_to_name(err));
        // continue anyway
    }

    if (dns2 && strlen(dns2) > 0)
    {
        dns_info.ip.u_addr.ip4.addr = ipaddr_addr(dns2);
        err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set backup DNS: %s", esp_err_to_name(err));
        }
    }

    // Save settings to NVS
    nconfig_write(NETIF_TYPE, "static");
    nconfig_write(NETIF_IP, ip);
    nconfig_write(NETIF_GATEWAY, gw);
    nconfig_write(NETIF_SUBNET, netmask);
    nconfig_write(NETIF_DNS1, dns1);
    nconfig_write(NETIF_DNS2, dns2);

    return ESP_OK;
}

esp_err_t wifi_sta_set_ap(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "Setting new AP with SSID: %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    wifi_control_lock();
    bool auto_reconnect = wifi_get_auto_reconnect();
    wifi_set_auto_reconnect(false);
    wifi_cancel_sta_connection();
    wifi_prepare_sta_disconnect();

    ESP_LOGI(TAG, "Disconnecting from current AP if connected.");
    esp_err_t err = esp_wifi_disconnect();
    bool disconnect_requested = err == ESP_OK;
    if (err == ESP_ERR_WIFI_NOT_CONNECT)
    {
        err = ESP_OK;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to disconnect from current AP: %s", esp_err_to_name(err));
        goto out;
    }

    if (disconnect_requested && !wifi_wait_for_sta_disconnect(WIFI_DISCONNECT_WAIT_MS))
        ESP_LOGI(TAG, "No STA disconnect event received; applying new Wi-Fi config");

    for (int attempt = 0; attempt < WIFI_CONFIG_RETRY_COUNT; ++attempt)
    {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_ERR_WIFI_STATE)
            break;

        vTaskDelay(pdMS_TO_TICKS(WIFI_CONFIG_RETRY_DELAY_MS));
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set Wi-Fi config: %s", esp_err_to_name(err));
        goto out;
    }

    err = nconfig_write(WIFI_SSID, ssid);
    if (err == ESP_OK)
        err = nconfig_write(WIFI_PASSWORD, password);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(err));
        goto out;
    }

    wifi_mark_sta_credentials_pending_validation();
    wifi_reset_reconnect_backoff();
    wifi_set_auto_reconnect(auto_reconnect);
    err = wifi_connect_locked();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to start connection to new AP: %s", esp_err_to_name(err));

out:
    wifi_set_auto_reconnect(auto_reconnect);
    wifi_control_unlock();
    return err;
}
