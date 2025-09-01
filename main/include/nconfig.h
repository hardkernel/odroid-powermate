//
// Created by shinys on 25. 7. 10.
//

#ifndef NCONFIG_H
#define NCONFIG_H

#include "nvs.h"
#include "esp_err.h"

#define NCONFIG_NVS_NAMESPACE "er"
#define NCONFIG_NOT_FOUND ESP_ERR_NVS_NOT_FOUND

esp_err_t init_nconfig();

enum nconfig_type
{
    WIFI_SSID,
    WIFI_PASSWORD,
    WIFI_MODE,
    AP_SSID,
    AP_PASSWORD,
    NETIF_HOSTNAME,
    NETIF_IP,
    NETIF_GATEWAY,
    NETIF_SUBNET,
    NETIF_DNS1,
    NETIF_DNS2,
    NETIF_TYPE,
    UART_BAUD_RATE,
    NCONFIG_TYPE_MAX,
};

bool nconfig_value_is_not_set(enum nconfig_type type);

// Write config
esp_err_t nconfig_write(enum nconfig_type type, const char* data);

// Check config is set and get config value length
esp_err_t nconfig_get_str_len(enum nconfig_type type, size_t* len);

// Read config
esp_err_t nconfig_read(enum nconfig_type type, char* data, size_t len);

// Remove key
esp_err_t nconfig_delete(enum nconfig_type type);

#endif //NCONFIG_H