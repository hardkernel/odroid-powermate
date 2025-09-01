//
// Created by shinys on 25. 7. 10.
//

#ifndef WIFI_H
#define WIFI_H
#include "esp_err.h"
#include "esp_netif_types.h"
#include "esp_wifi_types_generic.h"

/**
 * @brief Initializes the Wi-Fi driver, network interfaces, and event handlers.
 */
void wifi_init(void);

/**
 * @brief Converts a Wi-Fi authentication mode enum to its string representation.
 * @param mode The Wi-Fi authentication mode.
 * @return A string describing the authentication mode.
 */
const char* auth_mode_str(wifi_auth_mode_t mode);

/**
 * @brief Converts a Wi-Fi disconnection reason enum to its string representation.
 * @param reason The reason for disconnection.
 * @return A string describing the reason.
 */
const char* wifi_reason_str(wifi_err_reason_t reason);

/**
 * @brief Connects the device to the configured Wi-Fi AP in STA mode.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_connect(void);

/**
 * @brief Disconnects the device from the current Wi-Fi AP in STA mode.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_disconnect(void);

/**
 * @brief Scans for available Wi-Fi access points.
 * @param ap_records A pointer to store the found AP records.
 * @param count A pointer to store the number of found APs.
 */
void wifi_scan_aps(wifi_ap_record_t **ap_records, uint16_t *count);

/**
 * @brief Gets information about the currently connected access point.
 * @param ap_info A pointer to a structure to store the AP information.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_get_current_ap_info(wifi_ap_record_t *ap_info);

/**
 * @brief Gets the current IP information for the STA interface.
 * @param ip_info A pointer to a structure to store the IP information.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_get_current_ip_info(esp_netif_ip_info_t *ip_info);

/**
 * @brief Gets the DNS server information for the STA interface.
 * @param type The type of DNS server (main, backup, fallback).
 * @param dns_info A pointer to a structure to store the DNS information.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_get_dns_info(esp_netif_dns_type_t type, esp_netif_dns_info_t *dns_info);

/**
 * @brief Configures the STA interface to use DHCP.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_use_dhcp(void);

/**
 * @brief Configures the STA interface to use a static IP address.
 * @param ip The static IP address.
 * @param gw The gateway address.
 * @param netmask The subnet mask.
 * @param dns1 The primary DNS server.
 * @param dns2 The secondary DNS server (optional).
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_use_static(const char* ip, const char* gw, const char* netmask, const char* dns1, const char* dns2);

/**
 * @brief Switches the Wi-Fi operating mode (e.g., sta, apsta).
 * @param mode The target Wi-Fi mode as a string ("sta" or "apsta").
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_switch_mode(const char* mode);

/**
 * @brief Initializes the SNTP service for time synchronization.
 */
void initialize_sntp(void);

/**
 * @brief Starts the SNTP time synchronization process.
 */
void sync_time();

/**
 * @brief Sets the SSID and password for the STA mode, saves them to NVS, and connects to the AP.
 * @param ssid The SSID of the access point.
 * @param password The password of the access point.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_sta_set_ap(const char* ssid, const char* password);

#endif //WIFI_H
