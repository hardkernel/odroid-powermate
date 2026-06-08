#include <stdlib.h>
#include <string.h>
#include "auth.h"
#include "cJSON.h"
#include "climit.h"
#include "event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "monitor.h"
#include "nconfig.h"
#include "webserver.h"
#include "wifi.h"

static const char* TAG = "webserver";

typedef esp_err_t (*climit_set_fn_t)(double value);
typedef esp_err_t (*climit_get_fn_t)(float* limit_a, uint16_t* raw);

static const char* climit_display_name(const char* name)
{
    if (strcmp(name, "vin") == 0)
        return "VIN";
    if (strcmp(name, "main") == 0)
        return "MAIN";
    if (strcmp(name, "usb") == 0)
        return "USB";
    return name;
}

static bool handle_climit_item(cJSON* item, const char* name, double max_value, enum nconfig_type config_type,
                               climit_set_fn_t set_fn, climit_get_fn_t get_fn, cJSON* results, bool* any_success,
                               bool* any_failure)
{
    if (!item)
        return false;

    const char* display_name = climit_display_name(name);
    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(results, name, result);

    if (!cJSON_IsNumber(item))
    {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error", "not a number");
        ESP_LOGW(TAG, "%s current limit request rejected: value is not a number", display_name);
        push_eventf(EV_WARNING, "%s current limit request rejected: value is not a number", display_name);
        *any_failure = true;
        return true;
    }

    double val = item->valuedouble;
    cJSON_AddNumberToObject(result, "requested_a", val);

    if (val < 0.0 || val > max_value)
    {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error", "out of range");
        cJSON_AddNumberToObject(result, "max_a", max_value);
        ESP_LOGW(TAG, "%s current limit request rejected: requested=%.3fA, max=%.3fA", display_name, val,
                 max_value);
        push_eventf(EV_WARNING, "%s current limit request rejected: requested=%.3fA, max=%.3fA", display_name, val,
                    max_value);
        *any_failure = true;
        return true;
    }

    esp_err_t err = set_fn(val);
    if (err != ESP_OK)
    {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s current limit set failed: requested=%.3fA, error=%s", display_name, val,
                 esp_err_to_name(err));
        push_eventf(EV_WARNING, "%s current limit set failed: requested=%.3fA, error=%s", display_name, val,
                    esp_err_to_name(err));
        *any_failure = true;
        return true;
    }

    float applied_a = 0.0f;
    uint16_t raw = 0;
    err = get_fn(&applied_a, &raw);
    if (err != ESP_OK)
    {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s current limit readback failed: requested=%.3fA, error=%s", display_name, val,
                 esp_err_to_name(err));
        push_eventf(EV_WARNING, "%s current limit readback failed: requested=%.3fA, error=%s", display_name, val,
                    esp_err_to_name(err));
        *any_failure = true;
        return true;
    }

    char num_buf[10];
    snprintf(num_buf, sizeof(num_buf), "%.2f", val);
    err = nconfig_write(config_type, num_buf);
    if (err != ESP_OK)
    {
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
        cJSON_AddNumberToObject(result, "applied_a", applied_a);
        cJSON_AddNumberToObject(result, "raw", raw);
        ESP_LOGW(TAG, "%s current limit save failed: requested=%.3fA, applied=%.3fA, error=%s", display_name, val,
                 applied_a, esp_err_to_name(err));
        push_eventf(EV_WARNING, "%s current limit save failed: requested=%.3fA, applied=%.3fA, error=%s",
                    display_name, val, applied_a, esp_err_to_name(err));
        *any_failure = true;
        return true;
    }

    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "applied_a", applied_a);
    cJSON_AddNumberToObject(result, "raw", raw);
    *any_success = true;
    return true;
}

static esp_err_t setting_get_handler(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
    {
        return err;
    }

    wifi_ap_record_t ap_info;
    cJSON* root = cJSON_CreateObject();

    char buf[16];
    if (nconfig_read(WIFI_MODE, buf, sizeof(buf)) == ESP_OK)
    {
        cJSON_AddStringToObject(root, "mode", buf);
    }
    else
    {
        cJSON_AddStringToObject(root, "mode", "sta"); // Default to sta
    }

    if (nconfig_read(NETIF_TYPE, buf, sizeof(buf)) == ESP_OK)
    {
        cJSON_AddStringToObject(root, "net_type", buf);
    }
    else
    {
        cJSON_AddStringToObject(root, "net_type", "dhcp"); // Default to dhcp
    }

    if (nconfig_read(UART_BAUD_RATE, buf, sizeof(buf)) == ESP_OK)
    {
        cJSON_AddStringToObject(root, "baudrate", buf);
    }

    if (nconfig_read(SENSOR_PERIOD_MS, buf, sizeof(buf)) == ESP_OK)
    {
        cJSON_AddStringToObject(root, "period", buf);
    }

    // Add current limits to the response
    if (nconfig_read(VIN_CURRENT_LIMIT, buf, sizeof(buf)) == ESP_OK)
    {
        cJSON_AddNumberToObject(root, "vin_current_limit", atof(buf));
    }
    if (nconfig_read(MAIN_CURRENT_LIMIT, buf, sizeof(buf)) == ESP_OK)
    {
        cJSON_AddNumberToObject(root, "main_current_limit", atof(buf));
    }
    if (nconfig_read(USB_CURRENT_LIMIT, buf, sizeof(buf)) == ESP_OK)
    {
        cJSON_AddNumberToObject(root, "usb_current_limit", atof(buf));
    }

    if (wifi_get_current_ap_info(&ap_info) == ESP_OK)
    {
        cJSON_AddBoolToObject(root, "connected", true);
        cJSON_AddStringToObject(root, "ssid", (const char*)ap_info.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);

        esp_netif_ip_info_t ip_info;
        cJSON* ip_obj = cJSON_CreateObject();
        if (wifi_get_current_ip_info(&ip_info) == ESP_OK)
        {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(ip_obj, "ip", ip_str);
            esp_ip4addr_ntoa(&ip_info.gw, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(ip_obj, "gateway", ip_str);
            esp_ip4addr_ntoa(&ip_info.netmask, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(ip_obj, "subnet", ip_str);
        }

        esp_netif_dns_info_t dns_info;
        char dns_str[16];
        if (wifi_get_dns_info(ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK)
        {
            esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns_str, sizeof(dns_str));
            cJSON_AddStringToObject(ip_obj, "dns1", dns_str);
        }
        if (wifi_get_dns_info(ESP_NETIF_DNS_BACKUP, &dns_info) == ESP_OK)
        {
            esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns_str, sizeof(dns_str));
            cJSON_AddStringToObject(ip_obj, "dns2", dns_str);
        }
        cJSON_AddItemToObject(root, "ip", ip_obj);
    }
    else
    {
        cJSON_AddBoolToObject(root, "connected", false);
    }

    const char* json_string = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    free((void*)json_string);

    return ESP_OK;
}

static esp_err_t wifi_scan(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
    {
        return err;
    }

    wifi_ap_record_t* ap_records;
    uint16_t count;

    wifi_scan_aps(&ap_records, &count);

    cJSON* root = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
    {
        cJSON* ap_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(ap_obj, "ssid", (const char*)ap_records[i].ssid);
        cJSON_AddNumberToObject(ap_obj, "rssi", ap_records[i].rssi);
        cJSON_AddStringToObject(ap_obj, "authmode", auth_mode_str(ap_records[i].authmode));
        cJSON_AddItemToArray(root, ap_obj);
    }

    if (count > 0)
        free(ap_records);


    const char* json_string = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    free((void*)json_string);

    return ESP_OK;
}

static esp_err_t setting_post_handler(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
    {
        return err;
    }

    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);

    if (received <= 0)
    {
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
            httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* mode_item = cJSON_GetObjectItem(root, "mode");
    cJSON* net_type_item = cJSON_GetObjectItem(root, "net_type");
    cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON* baud_item = cJSON_GetObjectItem(root, "baudrate");
    cJSON* period_item = cJSON_GetObjectItem(root, "period");
    cJSON* vin_climit_item = cJSON_GetObjectItem(root, "vin_current_limit");
    cJSON* main_climit_item = cJSON_GetObjectItem(root, "main_current_limit");
    cJSON* usb_climit_item = cJSON_GetObjectItem(root, "usb_current_limit");
    cJSON* new_username_item = cJSON_GetObjectItem(root, "new_username");
    cJSON* new_password_item = cJSON_GetObjectItem(root, "new_password");

    bool action_taken = false;

    cJSON* resp_root = cJSON_CreateObject();

    if (mode_item && cJSON_IsString(mode_item))
    {
        const char* mode = mode_item->valuestring;
        ESP_LOGI(TAG, "Received mode switch request: %s", mode);

        if (strcmp(mode, "sta") == 0 || strcmp(mode, "apsta") == 0)
        {
            if (strcmp(mode, "apsta") == 0)
            {
                cJSON* ap_ssid_item = cJSON_GetObjectItem(root, "ap_ssid");
                cJSON* ap_pass_item = cJSON_GetObjectItem(root, "ap_password");

                if (ap_ssid_item && cJSON_IsString(ap_ssid_item))
                {
                    nconfig_write(AP_SSID, ap_ssid_item->valuestring);
                }

                if (ap_pass_item && cJSON_IsString(ap_pass_item))
                {
                    nconfig_write(AP_PASSWORD, ap_pass_item->valuestring);
                }
                else
                {
                    nconfig_delete(AP_PASSWORD);
                }
            }

            wifi_switch_mode(mode);
            cJSON_AddStringToObject(resp_root, "mode_status", "initiated");
            action_taken = true;
        }
    }

    if (net_type_item && cJSON_IsString(net_type_item))
    {
        const char* type = net_type_item->valuestring;
        ESP_LOGI(TAG, "Received network config: %s", type);

        if (strcmp(type, "static") == 0)
        {
            cJSON* ip_item = cJSON_GetObjectItem(root, "ip");
            cJSON* gw_item = cJSON_GetObjectItem(root, "gateway");
            cJSON* sn_item = cJSON_GetObjectItem(root, "subnet");
            cJSON* d1_item = cJSON_GetObjectItem(root, "dns1");
            cJSON* d2_item = cJSON_GetObjectItem(root, "dns2");

            const char* ip = cJSON_IsString(ip_item) ? ip_item->valuestring : NULL;
            const char* gw = cJSON_IsString(gw_item) ? gw_item->valuestring : NULL;
            const char* sn = cJSON_IsString(sn_item) ? sn_item->valuestring : NULL;
            const char* d1 = cJSON_IsString(d1_item) ? d1_item->valuestring : NULL;
            const char* d2 = cJSON_IsString(d2_item) ? d2_item->valuestring : NULL;

            if (ip && gw && sn && d1)
            {
                nconfig_write(NETIF_TYPE, "static");
                nconfig_write(NETIF_IP, ip);
                nconfig_write(NETIF_GATEWAY, gw);
                nconfig_write(NETIF_SUBNET, sn);
                nconfig_write(NETIF_DNS1, d1);
                if (d2)
                    nconfig_write(NETIF_DNS2, d2);
                else
                    nconfig_delete(NETIF_DNS2);

                wifi_use_static(ip, gw, sn, d1, d2);
                cJSON_AddStringToObject(resp_root, "net_status", "static_applied");
                action_taken = true;
            }
        }
        else if (strcmp(type, "dhcp") == 0)
        {
            nconfig_write(NETIF_TYPE, "dhcp");
            wifi_use_dhcp();
            cJSON_AddStringToObject(resp_root, "net_status", "dhcp_applied");
            action_taken = true;
        }
    }

    if (ssid_item && cJSON_IsString(ssid_item))
    {
        cJSON* pass_item = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(pass_item))
        {
            wifi_sta_set_ap(ssid_item->valuestring, pass_item->valuestring);
            cJSON_AddStringToObject(resp_root, "wifi_status", "connecting");
            action_taken = true;
        }
    }

    if (baud_item && cJSON_IsString(baud_item))
    {
        const char* baudrate = baud_item->valuestring;
        ESP_LOGI(TAG, "Received baudrate set request: %s", baudrate);
        nconfig_write(UART_BAUD_RATE, baudrate);
        change_baud_rate(strtol(baudrate, NULL, 10));
        cJSON_AddStringToObject(resp_root, "baudrate_status", "updated");
        action_taken = true;
    }

    if (period_item && cJSON_IsString(period_item))
    {
        const char* period_str = period_item->valuestring;
        ESP_LOGI(TAG, "Received period set request: %s", period_str);
        update_sensor_period(strtol(period_str, NULL, 10));
        cJSON_AddStringToObject(resp_root, "period_status", "updated");
        action_taken = true;
    }

    if (vin_climit_item || main_climit_item || usb_climit_item)
    {
        bool climit_success = false;
        bool climit_failure = false;
        cJSON* climit_results = cJSON_CreateObject();
        cJSON_AddItemToObject(resp_root, "climit_results", climit_results);

        handle_climit_item(vin_climit_item, "vin", VIN_CURRENT_LIMIT_MAX, VIN_CURRENT_LIMIT, climit_set_vin,
                           climit_get_vin, climit_results, &climit_success, &climit_failure);
        handle_climit_item(main_climit_item, "main", MAIN_CURRENT_LIMIT_MAX, MAIN_CURRENT_LIMIT, climit_set_main,
                           climit_get_main, climit_results, &climit_success, &climit_failure);
        handle_climit_item(usb_climit_item, "usb", USB_CURRENT_LIMIT_MAX, USB_CURRENT_LIMIT, climit_set_usb,
                           climit_get_usb, climit_results, &climit_success, &climit_failure);

        if (climit_failure)
        {
            cJSON_AddStringToObject(resp_root, "climit_status", climit_success ? "partial_error" : "error");
            cJSON_AddStringToObject(resp_root, "status", "error");
        }
        else
        {
            cJSON_AddStringToObject(resp_root, "climit_status", "updated");
        }
        action_taken = true;
    }

    if (new_username_item && cJSON_IsString(new_username_item) && new_password_item &&
        cJSON_IsString(new_password_item))
    {
        const char* new_username = new_username_item->valuestring;
        const char* new_password = new_password_item->valuestring;

        nconfig_write(PAGE_USERNAME, new_username);
        nconfig_write(PAGE_PASSWORD, new_password);
        ESP_LOGI(TAG, "Username and password updated successfully.");
        cJSON_AddStringToObject(resp_root, "auth_status", "updated");
        action_taken = true;
    }

    if (action_taken)
    {
        if (!cJSON_GetObjectItem(resp_root, "status"))
            cJSON_AddStringToObject(resp_root, "status", "ok");
        char* resp_str = cJSON_PrintUnformatted(resp_root);
        httpd_resp_sendstr(req, resp_str);
        free(resp_str);
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload or no known parameters");
    }

    cJSON_Delete(resp_root);
    cJSON_Delete(root);
    return ESP_OK;
}

void register_wifi_endpoint(httpd_handle_t server)
{
    httpd_uri_t status = {.uri = "/api/setting", .method = HTTP_GET, .handler = setting_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &status);

    httpd_uri_t set = {.uri = "/api/setting", .method = HTTP_POST, .handler = setting_post_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &set);

    httpd_uri_t scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan, .user_ctx = NULL};
    httpd_register_uri_handler(server, &scan);
}
