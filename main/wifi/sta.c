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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/def.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "nconfig.h"
#include "priv_wifi.h"
#include "wifi.h"

static const char* TAG = "STA";

#define WIFI_DISCONNECT_WAIT_MS 500
#define WIFI_CONFIG_RETRY_COUNT 10
#define WIFI_CONFIG_RETRY_DELAY_MS 100

// Room for the longest value of each field plus its NUL. NVS stores the NUL, so a buffer
// the size of the wifi_config_t field cannot read back a maximum-length SSID or PSK.
#define SSID_BUF (32 + 1)
#define PASSWORD_BUF (64 + 1)
#define IPV4_BUF 16 // "255.255.255.255"

static esp_err_t wifi_use_dhcp_locked(void);
static esp_err_t wifi_use_static_locked(const char* ip, const char* gw, const char* netmask,
                                        const char* dns1, const char* dns2);
static esp_err_t apply_static(uint32_t ip, uint32_t gw, uint32_t netmask, uint32_t dns1, uint32_t dns2);

// Why the saved static address was not used at this boot, or NULL.
static const char* s_static_ip_problem;

/**
 * Parses dotted decimal only: four octets of 0-255, no leading zeros, nothing else.
 * ipaddr_addr() also takes "10", "10.1" or octal and hex parts, and turns text it cannot
 * parse into 255.255.255.255. Returns the address in host byte order.
 */
static bool parse_ipv4(const char* s, uint32_t* out)
{
    if (!s)
        return false;
    uint32_t value = 0;
    for (int part = 0; part < 4; ++part)
    {
        if (part > 0 && *s++ != '.')
            return false;
        if (*s < '0' || *s > '9')
            return false;
        if (s[0] == '0' && s[1] >= '0' && s[1] <= '9')
            return false;
        unsigned octet = 0;
        for (int digits = 0; *s >= '0' && *s <= '9'; ++digits)
        {
            if (digits == 3)
                return false;
            octet = octet * 10 + (unsigned)(*s++ - '0');
        }
        if (octet > 255)
            return false;
        value = value << 8 | octet;
    }
    if (*s != '\0')
        return false;
    *out = value;
    return true;
}

// Not 0.0.0.0/8, loopback, multicast or reserved.
static bool is_unicast(uint32_t addr)
{
    uint8_t first = addr >> 24;
    return first != 0 && first != 127 && first < 224;
}

static bool refuse(const char** reason, const char* why)
{
    if (reason)
        *reason = why;
    return false;
}

// An optional address: absent, empty or 0.0.0.0 is none (0).
static bool parse_optional_ipv4(const char* s, uint32_t* out)
{
    *out = 0;
    if (!s || !s[0])
        return true;
    return parse_ipv4(s, out) && (*out == 0 || is_unicast(*out));
}

bool wifi_static_config_valid(const char* ip, const char* gw, const char* netmask, const char* dns1,
                              const char* dns2, const char** reason)
{
    uint32_t addr, gateway, mask, dns;
    if (!parse_ipv4(ip, &addr) || !is_unicast(addr))
        return refuse(reason, "ip is not a valid IPv4 host address");
    // 0.0.0.0, as a lease without a router option reports it, is a network without a router.
    if (!gw || !gw[0] || !parse_optional_ipv4(gw, &gateway))
        return refuse(reason, "gateway is not a valid IPv4 host address (0.0.0.0 for none)");
    if (!parse_ipv4(netmask, &mask))
        return refuse(reason, "subnet is not an IPv4 netmask");
    uint32_t host_bits = ~mask;
    if ((host_bits & (host_bits + 1)) != 0)
        return refuse(reason, "subnet is not a contiguous netmask");
    if (mask < 0xFF000000u || mask > 0xFFFFFFFCu)
        return refuse(reason, "subnet must be /8 to /30");
    if ((addr & host_bits) == 0 || (addr & host_bits) == host_bits)
        return refuse(reason, "ip is the network or broadcast address of its subnet");
    if (gateway != 0)
    {
        if ((gateway & mask) != (addr & mask))
            return refuse(reason, "gateway is not in the subnet of ip");
        if ((gateway & host_bits) == 0 || (gateway & host_bits) == host_bits)
            return refuse(reason, "gateway is the network or broadcast address of its subnet");
        if (gateway == addr)
            return refuse(reason, "ip and gateway are the same address");
    }
    if (!dns1 || !dns1[0] || !parse_optional_ipv4(dns1, &dns))
        return refuse(reason, "dns1 is not a valid IPv4 address (0.0.0.0 for none)");
    if (!parse_optional_ipv4(dns2, &dns))
        return refuse(reason, "dns2 is not a valid IPv4 address");
    return true;
}

bool wifi_ap_config_valid(const char* ssid, const char* password, const char** reason)
{
    if (ssid && (ssid[0] == '\0' || strlen(ssid) > 32))
        return refuse(reason, "ap_ssid must be 1 to 32 bytes");
    if (password && password[0])
    {
        size_t len = strlen(password);
        if (len < 8 || len > 63)
            return refuse(reason, "ap_password must be empty (open AP) or 8 to 63 characters");
        for (const char* p = password; *p; ++p)
            if (*p < 0x20 || *p > 0x7e)
                return refuse(reason, "ap_password must be printable ASCII");
    }
    return true;
}

bool wifi_sta_config_valid(const char* ssid, const char* password, const char** reason)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32)
        return refuse(reason, "ssid must be 1 to 32 bytes");
    size_t len = password ? strlen(password) : 0;
    if (len > 64)
        return refuse(reason, "password must be at most 63 characters, or 64 hex digits");
    if (len == 64)
        for (const char* p = password; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
                return refuse(reason, "a 64-character password must be a hex PSK");
    return true;
}

// Parses a saved address the way the firmware before v1.5.1 applied it (lwIP's
// ip4addr_aton, which also takes octal and hex parts and trailing spaces), so an update
// keeps a static address that worked. Returns network byte order, 0 when unusable.
// 255.255.255.255 is unusable except as a netmask: there the earlier firmware's
// ipaddr_addr() also made it of text it could not parse, and the /32 worked through the
// gateway.
static uint32_t parse_saved_ipv4(enum nconfig_type type, bool netmask)
{
    char buf[IPV4_BUF + 8] = "";
    if (nconfig_read(type, buf, sizeof(buf)) != ESP_OK)
        return 0;
    const char* start = buf;
    while (*start == ' ' || *start == '\t')
        ++start;
    ip4_addr_t addr;
    if (!ip4addr_aton(start, &addr))
    {
        if (!netmask)
            return 0;
        ESP_LOGW(TAG, "Saved netmask \"%s\" is not an IPv4 address; applying it as /32 like earlier firmware", buf);
        return IPADDR_NONE;
    }
    if (addr.addr == IPADDR_NONE && !netmask)
        return 0;
    return addr.addr;
}

/**
 * Applies the saved static address at boot. New settings pass wifi_static_config_valid();
 * saved ones may predate it, so only what the stack cannot do without is required here:
 * an address and a netmask. A bad gateway or DNS server is left out instead.
 */
static void apply_saved_static_config(void)
{
    uint32_t ip = parse_saved_ipv4(NETIF_IP, false);
    uint32_t netmask = parse_saved_ipv4(NETIF_SUBNET, true);
    if (ip == 0 || netmask == 0)
    {
        s_static_ip_problem = ip == 0 ? "the saved static address is missing or invalid"
                                      : "the saved static netmask is missing or invalid";
        ESP_LOGE(TAG, "Using DHCP instead of the static IP: %s", s_static_ip_problem);
        return;
    }
    ESP_LOGI(TAG, "Using static IP configuration for STA.");
    esp_err_t err = apply_static(ip, parse_saved_ipv4(NETIF_GATEWAY, false), netmask,
                                 parse_saved_ipv4(NETIF_DNS1, false), parse_saved_ipv4(NETIF_DNS2, false));
    if (err != ESP_OK)
    {
        s_static_ip_problem = "the static address could not be applied";
        ESP_LOGE(TAG, "Failed to apply the static IP: %s", esp_err_to_name(err));
    }
}

const char* wifi_static_ip_problem(void)
{
    return s_static_ip_problem;
}

const char* wifi_net_type_in_use(void)
{
    char buf[16] = "";
    bool saved_static = nconfig_read(NETIF_TYPE, buf, sizeof(buf)) == ESP_OK && strcmp(buf, "static") == 0;
    return saved_static && !s_static_ip_problem ? "static" : "dhcp";
}

/**
 * @brief Initializes and configures the STA mode.
 */
void wifi_init_sta(void)
{
    wifi_config_t wifi_config = {0};

    char ssid[SSID_BUF] = "";
    char password[PASSWORD_BUF] = "";
    if (nconfig_read(WIFI_SSID, ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0')
        ESP_LOGW(TAG, "STA SSID not configured in NVS.");
    if (nconfig_read(WIFI_PASSWORD, password, sizeof(password)) != ESP_OK)
        password[0] = '\0';
    // At most 32 and 64 bytes: the fields need no NUL at full length.
    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));
    memset(password, 0, sizeof(password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    memset(&wifi_config, 0, sizeof(wifi_config));
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to set the STA config: %s", esp_err_to_name(err));

    char netif_type[16] = "";
    s_static_ip_problem = NULL;
    if (nconfig_read(NETIF_TYPE, netif_type, sizeof(netif_type)) == ESP_OK && strcmp(netif_type, "static") == 0)
        apply_saved_static_config();

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
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        err = ESP_OK; // a saved static address the boot could not use left DHCP running
    if (err == ESP_OK)
    {
        nconfig_write(NETIF_TYPE, "dhcp");
        s_static_ip_problem = NULL;
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

// Clears a DNS server left from DHCP or an earlier static setting, in the TCP/IP task.
static esp_err_t clear_dns_server(void* ctx)
{
    dns_setserver((u8_t)(uintptr_t)ctx, NULL);
    return ESP_OK;
}

// Does not save anything: callers that take a new setting save it once it is applied.
static esp_err_t wifi_use_static_locked(const char* ip, const char* gw, const char* netmask, const char* dns1,
                                        const char* dns2)
{
    const char* reason = NULL;
    if (!wifi_static_config_valid(ip, gw, netmask, dns1, dns2, &reason))
    {
        ESP_LOGE(TAG, "Static IP refused: %s", reason);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t addr, gateway, mask, dns_main, dns_backup;
    parse_ipv4(ip, &addr);
    parse_optional_ipv4(gw, &gateway);
    parse_ipv4(netmask, &mask);
    parse_optional_ipv4(dns1, &dns_main);
    parse_optional_ipv4(dns2, &dns_backup);
    esp_err_t err = apply_static(lwip_htonl(addr), lwip_htonl(gateway), lwip_htonl(mask), lwip_htonl(dns_main),
                                 lwip_htonl(dns_backup));
    if (err == ESP_OK)
        s_static_ip_problem = NULL;
    return err;
}

// Addresses in network byte order; 0 for no gateway or no DNS server.
static esp_err_t apply_static(uint32_t ip, uint32_t gw, uint32_t netmask, uint32_t dns1, uint32_t dns2)
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

    esp_netif_ip_info_t ip_info = {
        .ip.addr = ip,
        .gw.addr = gw,
        .netmask.addr = netmask,
    };
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set static IP: %s", esp_err_to_name(err));
        return err;
    }

    // esp_netif_set_dns_info() refuses 0.0.0.0: "none" goes through lwIP instead.
    esp_netif_dns_info_t dns_info = {.ip.type = ESP_IPADDR_TYPE_V4};
    dns_info.ip.u_addr.ip4.addr = dns1;
    err = dns1 ? esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info)
               : esp_netif_tcpip_exec(clear_dns_server, (void*)ESP_NETIF_DNS_MAIN);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to set main DNS: %s", esp_err_to_name(err));

    dns_info.ip.u_addr.ip4.addr = dns2;
    err = dns2 ? esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info)
               : esp_netif_tcpip_exec(clear_dns_server, (void*)ESP_NETIF_DNS_BACKUP);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to set backup DNS: %s", esp_err_to_name(err));

    return ESP_OK;
}

esp_err_t wifi_sta_set_ap(const char* ssid, const char* password)
{
    const char* reason = NULL;
    if (!wifi_sta_config_valid(ssid, password, &reason))
    {
        ESP_LOGW(TAG, "Wi-Fi credentials refused: %s", reason);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Setting new AP with SSID: %s", ssid);

    // Lengths checked above: the fields need no NUL at 32 and 64 bytes.
    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    if (password)
        memcpy(wifi_config.sta.password, password, strlen(password));
    else
        password = "";

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
