/**
 * @file nconfig.h
 * @brief Provides an interface for managing system configuration using Non-Volatile Storage (NVS).
 */

#ifndef NCONFIG_H
#define NCONFIG_H

#include "esp_err.h"
#include "nvs.h"

#define NCONFIG_NVS_NAMESPACE "er" ///< The NVS namespace where all configurations are stored.
#define NCONFIG_NOT_FOUND ESP_ERR_NVS_NOT_FOUND ///< Alias for the NVS not found error code.

/**
 * @brief Initializes the nconfig module.
 *
 * This function initializes the Non-Volatile Storage (NVS) component, which is required
 * for all other nconfig operations. It should be called once at application startup.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t init_nconfig();

/**
 * @brief Defines the keys for all configuration values managed by nconfig.
 */
enum nconfig_type
{
    WIFI_SSID,          ///< The SSID of the Wi-Fi network to connect to (STA mode).
    WIFI_PASSWORD,      ///< The password for the Wi-Fi network (STA mode).
    WIFI_MODE,          ///< The Wi-Fi operating mode (e.g., "sta" or "apsta").
    AP_SSID,            ///< The SSID for the device's Access Point mode.
    AP_PASSWORD,        ///< The password for the device's Access Point mode.
    NETIF_HOSTNAME,     ///< The hostname of the device on the network.
    NETIF_IP,           ///< The static IP address for the STA interface.
    NETIF_GATEWAY,      ///< The gateway address for the STA interface.
    NETIF_SUBNET,       ///< The subnet mask for the STA interface.
    NETIF_DNS1,         ///< The primary DNS server address.
    NETIF_DNS2,         ///< The secondary DNS server address.
    NETIF_TYPE,         ///< The network interface type (e.g., "dhcp" or "static").
    UART_BAUD_RATE,     ///< The baud rate for the UART communication.
    VIN_CURRENT_LIMIT, ///< The maximum current limit for the VIN.
    MAIN_CURRENT_LIMIT, ///< The maximum current limit for the MAIN out.
    USB_CURRENT_LIMIT, ///< The maximum current limit for the USB out.
    PAGE_USERNAME, ///< Webpage username
    PAGE_PASSWORD, ///< Webpage password
    SENSOR_PERIOD_MS, ///< Sensor period
    NCONFIG_TYPE_MAX,   ///< Sentinel for the maximum number of configuration types.
};

/**
 * @brief Erase all of nvs data and restart system
 */
void reset_nconfig();

/**
 * @brief Checks if a specific configuration value has been set.
 *
 * @param type The configuration key to check.
 * @return True if the value is not set, false otherwise.
 */
bool nconfig_value_is_not_set(enum nconfig_type type);

/**
 * @brief Writes a configuration value to NVS.
 *
 * @param type The configuration key to write.
 * @param data A pointer to the string data to be stored.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t nconfig_write(enum nconfig_type type, const char* data);

/**
 * @brief Gets the length of a stored string configuration value.
 *
 * @param type The configuration key to query.
 * @param[out] len A pointer to a size_t variable to store the length.
 * @return ESP_OK on success, NCONFIG_NOT_FOUND if the key doesn't exist, or another error code.
 */
esp_err_t nconfig_get_str_len(enum nconfig_type type, size_t* len);

/**
 * @brief Reads a configuration value from NVS.
 *
 * @param type The configuration key to read.
 * @param[out] data A buffer to store the read data.
 * @param len The length of the buffer. It should be large enough to hold the value.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t nconfig_read(enum nconfig_type type, char* data, size_t len);

/**
 * @brief Deletes a configuration key-value pair from NVS.
 *
 * @param type The configuration key to delete.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t nconfig_delete(enum nconfig_type type);

#endif // NCONFIG_H
