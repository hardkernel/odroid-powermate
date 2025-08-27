//
// Created by shinys on 25. 7. 10.
//

#include "nconfig.h"

#include "nvs_flash.h"
#include "esp_err.h"

static nvs_handle_t handle;

const static char *keys[NCONFIG_TYPE_MAX] = {
    [WIFI_SSID] = "wifi_ssid",
    [WIFI_PASSWORD] = "wifi_pw",
    [WIFI_MODE] = "wifi_mode",
    [AP_SSID] = "ap_ssid",
    [AP_PASSWORD] = "ap_pw",
    [NETIF_HOSTNAME] = "hostname",
    [NETIF_IP] = "ip",
    [NETIF_GATEWAY] = "gw",
    [NETIF_SUBNET] = "sn",
    [NETIF_DNS1] = "dns1",
    [NETIF_DNS2] = "dns2",
    [NETIF_TYPE] = "dhcp",
    [UART_BAUD_RATE] = "baudrate",
};

struct default_value {
    enum nconfig_type type;
    const char *value;
};

struct default_value const default_values[] = {
    {WIFI_SSID, "HK_BOB_24G"},
    {WIFI_PASSWORD, ""},
    {NETIF_TYPE, "dhcp"},
    {NETIF_HOSTNAME, "odroid"},
    {UART_BAUD_RATE, "1500000"},
    {NETIF_DNS1, "8.8.8.8"},
    {NETIF_DNS2, "8.8.4.4"},
    {WIFI_MODE, "apsta"},
    {AP_SSID, "odroid-pm"},
    {AP_PASSWORD, "powermate"},
};

esp_err_t init_nconfig()
{
    esp_err_t ret = nvs_open(NCONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < sizeof(default_values) / sizeof(default_values[0]); ++i) {
        // check key is not exist or value is null
        size_t len = 0;
        nconfig_get_str_len(default_values[i].type, &len);
        if (len <= 1) // nconfig_get_str_len return err or value is '\0'
        {
            if (nconfig_write(default_values[i].type, default_values[i].value) != ESP_OK) // if nconfig write fail, system panic
                return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t nconfig_write(enum nconfig_type type, const char* data)
{
    return nvs_set_str(handle, keys[type], data);
}

esp_err_t nconfig_delete(enum nconfig_type type)
{
    return nvs_erase_key(handle, keys[type]);
}

esp_err_t nconfig_get_str_len(enum nconfig_type type, size_t *len)
{
    return nvs_get_str(handle, keys[type], NULL, len);
}

esp_err_t nconfig_read(enum nconfig_type type, char* data, size_t len)
{
    return nvs_get_str(handle, keys[type], data, &len);
}

esp_err_t nconfig_read_bool(enum nconfig_type type, char* data, size_t len)
{
    return nvs_get_str(handle, keys[type], data, &len);
}