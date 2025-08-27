//
// Created by shinys on 25. 7. 10.
//

#include "wifi.h"

#include "nconfig.h"
#include "indicator.h"

#include <string.h>
#include <system.h>
#include <lwip/sockets.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "rom/ets_sys.h"

static const char *TAG = "odroid";
#define MAX_RETRY 10
#define MAX_SCAN 20


const char* auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode)
    {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA_WPA2_PSK";
    case WIFI_AUTH_ENTERPRISE:
        return "ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI_PSK";
    case WIFI_AUTH_OWE:
        return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:
        return "WPA3_ENT_192";
    case WIFI_AUTH_WPA3_EXT_PSK:
        return "WPA3_EXT_PSK";
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
        return "WPA3_EXT_PSK_MIXED_MODE";
    case WIFI_AUTH_DPP:
        return "DPP";
    case WIFI_AUTH_WPA3_ENTERPRISE:
        return "WPA3_ENTERPRISE";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
        return "WPA2_WPA3_ENTERPRISE";
    default:
        return "UNKNOWN";
    }
}

static const char* wifi_reason_str(wifi_err_reason_t reason) {
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
        case WIFI_REASON_NOT_AUTHED: return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "DISASSOC_PWRCAP_BAD";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "DISASSOC_SUPCHAN_BAD";
        case WIFI_REASON_IE_INVALID: return "IE_INVALID";
        case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
        case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
        case WIFI_REASON_AKMP_INVALID: return "AKMP_INVALID";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
        case WIFI_REASON_INVALID_RSN_IE_CAP: return "INVALID_RSN_IE_CAP";
        case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
        case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
        case WIFI_REASON_INVALID_PMKID: return "INVALID_PMKID";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
        case WIFI_REASON_ROAMING: return "ROAMING";
        default: return "UNKNOWN";
    }
}

static esp_netif_t *wifi_sta_netif = NULL;
static esp_netif_t *wifi_ap_netif = NULL;

static int s_retry_num = 0;

static esp_err_t wifi_sta_do_disconnect(void);

static void sntp_sync_time_cb(struct timeval *tv)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Time synchronized: %s", strftime_buf);
}

static void handler_on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    s_retry_num++;
    if (s_retry_num > MAX_RETRY) {
        ESP_LOGW(TAG, "WiFi Connect failed %d times, stop reconnect.", s_retry_num);
        /* let example_wifi_sta_do_connect() return */

        wifi_sta_do_disconnect();
        start_reboot_timer(60);
        return;
    }
    wifi_event_sta_disconnected_t *disconn = event_data;
    if (disconn->reason == WIFI_REASON_ROAMING) {
        ESP_LOGD(TAG, "station roaming, do nothing");
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi disconnected, reason: (%s)", wifi_reason_str(disconn->reason));
    ESP_LOGI(TAG, "Trying to reconnect...");
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return;
    }
    ESP_ERROR_CHECK(err);
}

static void handler_on_wifi_connect(void *esp_netif, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{

}

static void handler_on_sta_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    stop_reboot_timer();
    s_retry_num = 0;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (strcmp("sta", esp_netif_get_desc(event->esp_netif)) != 0) {
        return;
    }
    ESP_LOGI(TAG, "Got IPv4 event: Interface \"%s\" address: " IPSTR, esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "- IPv4 address: " IPSTR ",", IP2STR(&event->ip_info.ip));
    sync_time();
    led_set(LED_BLU, BLINK_SOLID);
}

static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}


static esp_err_t set_hostname(esp_netif_t* esp_netif, const char *hostname)
{
    if (esp_netif_set_hostname(esp_netif, hostname) != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}

static void wifi_start(wifi_mode_t mode)
{
    size_t hostname_len;
    char type_buf[16];

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
        wifi_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);

        if (nconfig_read(NETIF_TYPE, type_buf, sizeof(type_buf)) == ESP_OK && strcmp(type_buf, "static") == 0) {
            ESP_LOGI(TAG, "Using static IP configuration");
            char ip_buf[16], gw_buf[16], mask_buf[16], dns1_buf[16], dns2_buf[16];
            nconfig_read(NETIF_IP, ip_buf, sizeof(ip_buf));
            nconfig_read(NETIF_GATEWAY, gw_buf, sizeof(gw_buf));
            nconfig_read(NETIF_SUBNET, mask_buf, sizeof(mask_buf));
            const char* dns1 = (nconfig_read(NETIF_DNS1, dns1_buf, sizeof(dns1_buf)) == ESP_OK) ? dns1_buf : NULL;
            const char* dns2 = (nconfig_read(NETIF_DNS2, dns2_buf, sizeof(dns2_buf)) == ESP_OK) ? dns2_buf : NULL;
            if (dns1 == NULL)
                wifi_use_static(ip_buf, gw_buf, mask_buf, "8.8.8.8", "8.8.4.4");
            else
                wifi_use_static(ip_buf, gw_buf, mask_buf, dns1, dns2);
        } else {
            ESP_LOGI(TAG, "Using DHCP configuration");
            wifi_use_dhcp();
        }

        nconfig_get_str_len(NETIF_HOSTNAME, &hostname_len);
        char buf[hostname_len];
        nconfig_read(NETIF_HOSTNAME, buf, sizeof(buf));
        set_hostname(wifi_sta_netif, buf);
    }

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        esp_netif_inherent_config_t esp_netif_config_ap = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
        wifi_ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &esp_netif_config_ap);
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &wifi_ap_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &wifi_ap_event_handler, NULL));
    }

    esp_wifi_set_default_wifi_sta_handlers();

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_start());
}


static void wifi_stop(void)
{
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        return;
    }
    ESP_ERROR_CHECK(err);

    if (wifi_ap_netif) {
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &wifi_ap_event_handler);
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &wifi_ap_event_handler);
    }

    ESP_ERROR_CHECK(esp_wifi_deinit());

    if (wifi_sta_netif) {
        esp_netif_destroy(wifi_sta_netif);
        wifi_sta_netif = NULL;
    }
    if (wifi_ap_netif) {
        esp_netif_destroy(wifi_ap_netif);
        wifi_ap_netif = NULL;
    }
}


static esp_err_t wifi_sta_do_connect(wifi_config_t wifi_config)
{
    stop_reboot_timer();
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, handler_on_wifi_disconnect, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handler_on_sta_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, handler_on_wifi_connect, wifi_sta_netif));


    ESP_LOGI(TAG, "Connecting to %s...", (char*)wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed! ret:%x", ret);
        return ret;
    }
    return ESP_OK;
}

static esp_err_t wifi_sta_do_disconnect(void)
{
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &handler_on_wifi_disconnect));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_on_sta_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &handler_on_wifi_connect));
    led_set(LED_BLU, BLINK_DOUBLE);

    return esp_wifi_disconnect();
}

static void wifi_shutdown(void)
{
    wifi_sta_do_disconnect();
    wifi_stop();
}

static esp_err_t do_connect(void)
{
    esp_err_t err;
    char mode_buf[16] = {0};
    wifi_mode_t mode = WIFI_MODE_STA; // Default mode

    if (nconfig_read(WIFI_MODE, mode_buf, sizeof(mode_buf)) == ESP_OK) {
        if (strcmp(mode_buf, "apsta") == 0) {
            mode = WIFI_MODE_APSTA;
            ESP_LOGI(TAG, "Starting in APSTA mode");
        } else { // "sta" or anything else defaults to STA
            mode = WIFI_MODE_STA;
            ESP_LOGI(TAG, "Starting in STA mode");
        }
    } else {
        ESP_LOGI(TAG, "WIFI_MODE not set, defaulting to STA mode");
    }

    wifi_start(mode);

    // Configure and connect STA interface if needed
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_config_t sta_config = {0};
        bool sta_creds_ok = false;
        if (nconfig_read(WIFI_SSID, (char*)sta_config.sta.ssid, 32) == ESP_OK && strlen((char*)sta_config.sta.ssid) > 0) {
            if (nconfig_read(WIFI_PASSWORD, (char*)sta_config.sta.password, 64) == ESP_OK) {
                sta_creds_ok = true;
            }
        }

        if (sta_creds_ok) {
            err = wifi_sta_do_connect(sta_config);
            if (err != ESP_OK && mode == WIFI_MODE_STA) {
                // In STA-only mode, failure to connect is a fatal error
                return err;
            }
        } else if (mode == WIFI_MODE_STA) {
            // In STA-only mode, missing credentials is a fatal error
            ESP_LOGE(TAG, "Missing STA credentials in STA mode.");
            return ESP_FAIL;
        } else {
            // In APSTA mode, missing credentials is a warning
            ESP_LOGW(TAG, "Missing STA credentials in APSTA mode. STA will not connect.");
        }
    }

    // Configure AP interface if needed
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        char ap_ssid[32], ap_pass[64];
        wifi_config_t ap_config = {
            .ap = {
                .channel = 1,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK,
                .ssid_hidden = 0,
            },
        };

        if (nconfig_read(AP_SSID, ap_ssid, sizeof(ap_ssid)) == ESP_OK && strlen(ap_ssid) > 0) {
            strcpy((char*)ap_config.ap.ssid, ap_ssid);
        } else {
            strcpy((char*)ap_config.ap.ssid, "ODROID-REMOTE-AP");
        }

        if (nconfig_read(AP_PASSWORD, ap_pass, sizeof(ap_pass)) == ESP_OK && strlen(ap_pass) >= 8) {
            strcpy((char*)ap_config.ap.password, ap_pass);
        } else {
            ap_config.ap.authmode = WIFI_AUTH_OPEN;
            memset(ap_config.ap.password, 0, sizeof(ap_config.ap.password));
        }
        ap_config.ap.ssid_len = strlen((char*)ap_config.ap.ssid);

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_LOGI(TAG, "AP configured, SSID: %s", ap_config.ap.ssid);
    }

    return ESP_OK;
}

esp_err_t wifi_connect(void)
{
    led_set(LED_BLU, BLINK_DOUBLE);

    static esp_sntp_config_t ntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
        ESP_SNTP_SERVER_LIST("time.windows.com", "pool.ntp.org", "216.239.35.0")); // google public ntp
    ntp_cfg.start = false;
    ntp_cfg.sync_cb = sntp_sync_time_cb;
    ntp_cfg.smooth_sync = true; // Sync immediately when started
    esp_netif_sntp_init(&ntp_cfg);

    if (do_connect() != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_register_shutdown_handler(&wifi_shutdown));
    return ESP_OK;
}


esp_err_t wifi_disconnect(void)
{
    wifi_shutdown();
    ESP_ERROR_CHECK(esp_unregister_shutdown_handler(&wifi_shutdown));
    return ESP_OK;
}

void wifi_scan_aps(wifi_ap_record_t **ap_records, uint16_t* count)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        *count = 0;
        *ap_records = NULL;
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(count));
    ESP_LOGI(TAG, "Found %u access points", *count);

    if (*count == 0)
        *ap_records = NULL;
    else
        *ap_records = calloc(*count, sizeof(wifi_ap_record_t));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(count, *ap_records));
    ESP_LOGI(TAG, "Scan done");
}

esp_err_t wifi_get_current_ap_info(wifi_ap_record_t *ap_info)
{
    esp_err_t ret = esp_wifi_sta_get_ap_info(ap_info);
    if (ret != ESP_OK) {
        // Clear ssid and set invalid rssi on error
        memset(ap_info->ssid, 0, sizeof(ap_info->ssid));
        ap_info->rssi = -127;
    }
    return ret;
}

esp_err_t wifi_get_current_ip_info(esp_netif_ip_info_t *ip_info)
{
    return esp_netif_get_ip_info(wifi_sta_netif, ip_info);
}

esp_err_t wifi_get_dns_info(esp_netif_dns_type_t type, esp_netif_dns_info_t *dns_info)
{
    if (wifi_sta_netif) {
        return esp_netif_get_dns_info(wifi_sta_netif, type, dns_info);
    }
    return ESP_FAIL;
}

esp_err_t wifi_use_static(const char *ip, const char *gw, const char *netmask, const char *dns1, const char *dns2)
{
    if (wifi_sta_netif == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_netif_dhcpc_stop(wifi_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Failed to stop DHCP client: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip_info;
    inet_pton(AF_INET, ip, &ip_info.ip);
    inet_pton(AF_INET, gw, &ip_info.gw);
    inet_pton(AF_INET, netmask, &ip_info.netmask);
    err = esp_netif_set_ip_info(wifi_sta_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set static IP info: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_dns_info_t dns_info;
    if (dns1 && strlen(dns1) > 0) {
        inet_pton(AF_INET, dns1, &dns_info.ip.u_addr.ip4);
        esp_netif_set_dns_info(wifi_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    } else {
        esp_netif_set_dns_info(wifi_sta_netif, ESP_NETIF_DNS_MAIN, NULL);
    }

    if (dns2 && strlen(dns2) > 0) {
        inet_pton(AF_INET, dns2, &dns_info.ip.u_addr.ip4);
        esp_netif_set_dns_info(wifi_sta_netif, ESP_NETIF_DNS_BACKUP, &dns_info);
    } else {
        esp_netif_set_dns_info(wifi_sta_netif, ESP_NETIF_DNS_BACKUP, NULL);
    }

    return ESP_OK;
}

esp_err_t wifi_use_dhcp(void)
{
    if (wifi_sta_netif == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_netif_dhcpc_start(wifi_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "Failed to start DHCP client: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t wifi_switch_mode(const char* mode)
{
    if (strcmp(mode, "sta") != 0 && strcmp(mode, "apsta") != 0) {
        ESP_LOGE(TAG, "Invalid mode specified: %s. Use 'sta' or 'apsta'.", mode);
        return ESP_ERR_INVALID_ARG;
    }

    char current_mode_buf[16] = {0};
    if (nconfig_read(WIFI_MODE, current_mode_buf, sizeof(current_mode_buf)) == ESP_OK) {
        if (strcmp(current_mode_buf, mode) == 0) {
            ESP_LOGI(TAG, "Already in %s mode.", mode);
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Switching Wi-Fi mode to %s.", mode);

    esp_err_t err = nconfig_write(WIFI_MODE, mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save new Wi-Fi mode to NVS");
        return err;
    }

    wifi_disconnect();
    err = wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect in new mode %s", mode);
        return err;
    }

    ESP_LOGI(TAG, "Successfully switched to %s mode.", mode);
    return ESP_OK;
}

void sync_time()
{
    esp_netif_sntp_start();
    ESP_LOGI(TAG, "SNTP service started, waiting for time synchronization...");
}