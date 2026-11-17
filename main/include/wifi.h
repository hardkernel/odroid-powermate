/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by shinys on 25. 7. 10.
//

#ifndef WIFI_H
#define WIFI_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_types.h"
#include "esp_wifi_types_generic.h"

/**
 * @brief Initializes the Wi-Fi driver, network interfaces, and event handlers.
 */
void wifi_init(void);

/**
 * @brief The device's network name: CONFIG_LWIP_LOCAL_HOSTNAME followed by the last two
 *        bytes of the Wi-Fi station MAC, e.g. "odroid-pm-a1b2". DHCP and mDNS both use it,
 *        so several PowerMates on one network get distinct names.
 */
const char* wifi_hostname(void);

/**
 * @brief Advertises the PowerMate over mDNS as <wifi_hostname()>.local with an HTTP
 *        service and a _powermate._tcp service for discovery tools. Call once after
 *        wifi_init(). Failure is logged and not fatal.
 */
esp_err_t discovery_start(void);

/**
 * @brief Points the advertised web service at the server that actually started: with HTTPS
 * on, _https._tcp on port 443 replaces _http._tcp, and _powermate._tcp moves to port 443
 * with TXT tls=1.
 */
void discovery_set_web(bool https);

typedef enum
{
    WIFI_STA_CONNECTION_IDLE,
    WIFI_STA_CONNECTION_CONNECTING,
    WIFI_STA_CONNECTION_CONNECTED,
    WIFI_STA_CONNECTION_FAILED,
} wifi_sta_connection_state_t;

typedef struct
{
    wifi_sta_connection_state_t connection_state;
    wifi_err_reason_t last_disconnect_reason;
    uint32_t reconnect_backoff_ms;
    uint32_t last_connected_uptime_seconds;
    bool has_connected;
} wifi_sta_diagnostics_t;

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
void wifi_scan_aps(wifi_ap_record_t** ap_records, uint16_t* count);

/**
 * @brief Gets information about the currently connected access point.
 * @param ap_info A pointer to a structure to store the AP information.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_get_current_ap_info(wifi_ap_record_t* ap_info);

/**
 * @brief Gets the current IP information for the STA interface.
 * @param ip_info A pointer to a structure to store the IP information.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_get_current_ip_info(esp_netif_ip_info_t* ip_info);

/**
 * @brief Gets the current STA connection attempt state.
 */
wifi_sta_connection_state_t wifi_get_sta_connection_state(void);

/**
 * @brief Gets the reason for the last terminal STA connection failure.
 */
wifi_err_reason_t wifi_get_sta_connection_failure_reason(void);

/**
 * @brief Gets a snapshot of the current STA diagnostic state.
 */
void wifi_get_sta_diagnostics(wifi_sta_diagnostics_t* diagnostics);

/**
 * @brief Gets the DNS server information for the STA interface.
 * @param type The type of DNS server (main, backup, fallback).
 * @param dns_info A pointer to a structure to store the DNS information.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_get_dns_info(esp_netif_dns_type_t type, esp_netif_dns_info_t* dns_info);

/**
 * @brief Configures the STA interface to use DHCP.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t wifi_use_dhcp(void);

/**
 * @brief Checks a static STA configuration before it is applied or saved.
 *
 * Every address is dotted decimal (four octets, no leading zeros); the netmask is
 * contiguous, /8 to /30; the address and the gateway are distinct hosts of that subnet.
 * A gateway or dns1 of 0.0.0.0 means none (a network without a router or DNS server);
 * dns2 may also be NULL or empty for none.
 * @param[out] reason The first problem found, when the configuration is refused (optional).
 * @return true when the configuration can be used.
 */
bool wifi_static_config_valid(const char* ip, const char* gw, const char* netmask, const char* dns1,
                              const char* dns2, const char** reason);

/**
 * @brief Why the saved static address is not in use since the last boot or mode switch
 * (the device fell back to DHCP), or NULL.
 */
const char* wifi_static_ip_problem(void);

/** @brief "static" or "dhcp": the STA addressing actually in use. */
const char* wifi_net_type_in_use(void);

/**
 * @brief Checks the setup AP's SSID and password: SSID 1-32 bytes; password empty (open
 * AP) or 8-63 printable ASCII characters (WPA2). Either may be NULL to leave it unchecked.
 */
bool wifi_ap_config_valid(const char* ssid, const char* password, const char** reason);

/**
 * @brief Checks the credentials of the network to join: SSID 1-32 bytes; password empty,
 * up to 63 characters, or 64 hex digits (a raw PSK).
 */
bool wifi_sta_config_valid(const char* ssid, const char* password, const char** reason);

/**
 * @brief Configures the STA interface to use a static IP address. Does not save anything.
 * @param ip The static IP address.
 * @param gw The gateway address.
 * @param netmask The subnet mask.
 * @param dns1 The primary DNS server.
 * @param dns2 The secondary DNS server, or NULL or "" for none.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if wifi_static_config_valid() refuses it,
 *         or another error code on failure.
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
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if wifi_sta_config_valid() refuses them,
 *         or another error code on failure.
 */
esp_err_t wifi_sta_set_ap(const char* ssid, const char* password);

#endif // WIFI_H
