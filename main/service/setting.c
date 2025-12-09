#include <stdlib.h>
#include "auth.h"
#include "cJSON.h"
#include "climit.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "monitor.h"
#include "nconfig.h"
#include "webserver.h"
#include "wifi.h"

static const char* TAG = "webserver";

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
                else
                {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP SSID required for APSTA mode");
                    cJSON_Delete(root);
                    return ESP_FAIL;
                }

                if (ap_pass_item && cJSON_IsString(ap_pass_item))
                {
                    nconfig_write(AP_PASSWORD, ap_pass_item->valuestring);
                }
                else
                {
                    nconfig_delete(AP_PASSWORD); // Open network
                }
            }

            wifi_switch_mode(mode);
            httpd_resp_sendstr(req, "{\"status\":\"mode_switch_initiated\"}");
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode");
        }
    }
    else if (net_type_item && cJSON_IsString(net_type_item))
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
                httpd_resp_sendstr(req, "{\"status\":\"static_config_applied\"}");
            }
            else
            {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing static IP fields");
            }
        }
        else if (strcmp(type, "dhcp") == 0)
        {
            nconfig_write(NETIF_TYPE, "dhcp");
            wifi_use_dhcp();
            httpd_resp_sendstr(req, "{\"status\":\"dhcp_config_applied\"}");
        }
    }
    else if (ssid_item && cJSON_IsString(ssid_item))
    {
        cJSON* pass_item = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(pass_item))
        {
            httpd_resp_sendstr(req, "{\"status\":\"connection_initiated\"}");

            wifi_sta_set_ap(ssid_item->valuestring, pass_item->valuestring);
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password required");
        }
    }
    else if (baud_item && cJSON_IsString(baud_item))
    {
        const char* baudrate = baud_item->valuestring;
        ESP_LOGI(TAG, "Received baudrate set request: %s", baudrate);
        nconfig_write(UART_BAUD_RATE, baudrate);
        change_baud_rate(strtol(baudrate, NULL, 10));
        httpd_resp_sendstr(req, "{\"status\":\"baudrate_updated\"}");
    }
    else if (period_item && cJSON_IsString(period_item))
    {
        const char* period_str = period_item->valuestring;
        ESP_LOGI(TAG, "Received period set request: %s", period_str);
        update_sensor_period(strtol(period_str, NULL, 10));
        httpd_resp_sendstr(req, "{\"status\":\"period_updated\"}");
    }
    else if (vin_climit_item || main_climit_item || usb_climit_item)
    {
        char num_buf[10];
        if (vin_climit_item && cJSON_IsNumber(vin_climit_item))
        {
            double val = vin_climit_item->valuedouble;
            if (val >= 0.0 && val <= VIN_CURRENT_LIMIT_MAX)
            {
                snprintf(num_buf, sizeof(num_buf), "%.2f", val);
                nconfig_write(VIN_CURRENT_LIMIT, num_buf);
                climit_set_vin(val);
            }
        }
        if (main_climit_item && cJSON_IsNumber(main_climit_item))
        {
            double val = main_climit_item->valuedouble;
            if (val >= 0.0 && val <= MAIN_CURRENT_LIMIT_MAX)
            {
                snprintf(num_buf, sizeof(num_buf), "%.2f", val);
                nconfig_write(MAIN_CURRENT_LIMIT, num_buf);
                climit_set_main(val);
            }
        }
        if (usb_climit_item && cJSON_IsNumber(usb_climit_item))
        {
            double val = usb_climit_item->valuedouble;
            if (val >= 0.0 && val <= USB_CURRENT_LIMIT_MAX)
            {
                snprintf(num_buf, sizeof(num_buf), "%.2f", val);
                nconfig_write(USB_CURRENT_LIMIT, num_buf);
                climit_set_usb(val);
            }
        }
        httpd_resp_sendstr(req, "{\"status\":\"current_limit_updated\"}");
    }
    else if (new_username_item && cJSON_IsString(new_username_item) && new_password_item &&
             cJSON_IsString(new_password_item))
    {
        const char* new_username = new_username_item->valuestring;
        const char* new_password = new_password_item->valuestring;

        nconfig_write(PAGE_USERNAME, new_username);
        nconfig_write(PAGE_PASSWORD, new_password);
        ESP_LOGI(TAG, "Username and password updated successfully.");
        httpd_resp_sendstr(req, "{\"status\":\"user_credentials_updated\"}");
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload");
    }

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
