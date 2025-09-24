//
// Created by shinys on 25. 7. 10.
//

#include "nconfig.h"

#include "esp_err.h"
#include "nvs_flash.h"

static nvs_handle_t handle;

const static char* keys[NCONFIG_TYPE_MAX] = {
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
    [VIN_CURRENT_LIMIT] = "vin_climit",
    [MAIN_CURRENT_LIMIT] = "main_climit",
    [USB_CURRENT_LIMIT] = "usb_climit",
};

struct default_value
{
    enum nconfig_type type;
    const char* value;
};

struct default_value const default_values[] = {
    {WIFI_SSID, ""},
    {WIFI_PASSWORD, ""},
    {NETIF_TYPE, "dhcp"},
    {NETIF_HOSTNAME, "powermate"},
    {UART_BAUD_RATE, "1500000"},
    {NETIF_DNS1, "8.8.8.8"},
    {NETIF_DNS2, "8.8.4.4"},
    {WIFI_MODE, "apsta"},
    {AP_SSID, "powermate"},
    {AP_PASSWORD, "hardkernel"},
    {VIN_CURRENT_LIMIT, "4.0"},
    {MAIN_CURRENT_LIMIT, "3.0"},
    {USB_CURRENT_LIMIT, "3.0"},
};

esp_err_t init_nconfig()
{
    esp_err_t ret = nvs_open(NCONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
        return ret;

    for (int i = 0; i < sizeof(default_values) / sizeof(default_values[0]); ++i)
    {
        // check key is not exist or value is null
        if (nconfig_value_is_not_set(default_values[i].type))
        {
            if (nconfig_write(default_values[i].type, default_values[i].value) != ESP_OK)
                // if nconfig write fail, system panic
                return ESP_FAIL;
        }
    }

    return ESP_OK;
}

bool nconfig_value_is_not_set(enum nconfig_type type)
{
    size_t len = 0;
    esp_err_t err = nconfig_get_str_len(type, &len);
    return (err != ESP_OK || len <= 1);
}

esp_err_t nconfig_write(enum nconfig_type type, const char* data) { return nvs_set_str(handle, keys[type], data); }

esp_err_t nconfig_delete(enum nconfig_type type) { return nvs_erase_key(handle, keys[type]); }

esp_err_t nconfig_get_str_len(enum nconfig_type type, size_t* len)
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
