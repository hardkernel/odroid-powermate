/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "webserver.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include "auth.h"
#include "cJSON.h"
#include "dbg_console.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "monitor.h"
#include "nconfig.h"
#include "system.h"

static const char* TAG = "WEBSERVER";

// The page is embedded in the firmware, so the firmware's ELF hash identifies it.
static const char* index_etag(void)
{
    static char etag[2 + 16 + 1];
    if (etag[0] == '\0')
    {
        char sha[16 + 1];
        esp_app_get_elf_sha256(sha, sizeof(sha));
        snprintf(etag, sizeof(etag), "\"%s\"", sha);
    }
    return etag;
}

static esp_err_t index_handler(httpd_req_t* req)
{
    extern const unsigned char index_html_start[] asm("_binary_index_html_gz_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_gz_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    const char* etag = index_etag();

    // no-cache lets the browser keep the page but makes it ask first; while the
    // firmware is unchanged the answer is a bodiless 304 instead of the whole page.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);

    char if_none_match[64];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, sizeof(if_none_match)) == ESP_OK &&
        strstr(if_none_match, etag) != NULL)
    {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_type(req, "text/html");
    // One send with a Content-Length: lwIP streams it straight from flash in full
    // segments, instead of a chunk header, 2 KB and a trailer per round.
    return httpd_resp_send(req, (const char*)index_html_start, index_html_size);
}

static esp_err_t login_handler(httpd_req_t* req)
{
    char content[100]; // Adjust size as needed for username/password
    int ret = httpd_req_recv(req, content, sizeof(content) - 1); // -1 for null terminator
    if (ret <= 0)
    { // 0 means connection closed, < 0 means error
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0'; // Null-terminate the received data

    cJSON* root = cJSON_Parse(content);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* username_json = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON* password_json = cJSON_GetObjectItemCaseSensitive(root, "password");

    if (!cJSON_IsString(username_json) || (username_json->valuestring == NULL) || !cJSON_IsString(password_json) ||
        (password_json->valuestring == NULL))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing username or password");
        return ESP_FAIL;
    }

    const char* received_username = username_json->valuestring;
    const char* received_password = password_json->valuestring;

    // Never log the password.
    ESP_LOGI(TAG, "Received login request for user: %s", received_username);

    // Get stored username and password from nconfig
    size_t stored_username_len = 0;
    size_t stored_password_len = 0;
    char* stored_username = NULL;
    char* stored_password = NULL;
    bool credentials_match = false;

    if (nconfig_get_str_len(PAGE_USERNAME, &stored_username_len) == ESP_OK && stored_username_len > 1)
    {
        stored_username = (char*)malloc(stored_username_len);
        if (stored_username)
        {
            if (nconfig_read(PAGE_USERNAME, stored_username, stored_username_len) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to read stored username from nconfig");
                free(stored_username);
                stored_username = NULL;
            }
        }
    }

    if (nconfig_get_str_len(PAGE_PASSWORD, &stored_password_len) == ESP_OK && stored_password_len > 1)
    {
        stored_password = (char*)malloc(stored_password_len);
        if (stored_password)
        {
            if (nconfig_read(PAGE_PASSWORD, stored_password, stored_password_len) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to read stored password from nconfig");
                free(stored_password);
                stored_password = NULL;
            }
        }
    }

    if (stored_username && stored_password)
    {
        if (strcmp(received_username, stored_username) == 0 && strcmp(received_password, stored_password) == 0)
        {
            credentials_match = true;
        }
    }

    if (stored_username)
        free(stored_username);
    if (stored_password)
        free(stored_password);

    if (credentials_match)
    {
        char* token = auth_generate_token();
        if (token)
        {
            cJSON* response_root = cJSON_CreateObject();
            cJSON_AddStringToObject(response_root, "token", token);
            char* json_response = cJSON_Print(response_root);

            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, json_response);

            free(token); // Free the token generated by auth_generate_token
            free(json_response);
            cJSON_Delete(response_root);
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate token");
        }
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid credentials");
    }

    cJSON_Delete(root);
    return ESP_OK;
}


// An sdkconfig generated before CONFIG_LWIP_MAX_SOCKETS was raised makes httpd_start fail,
// which leaves the device without a web UI and without current limits. sdkconfig is not
// refreshed from sdkconfig.defaults once it exists: delete it and rebuild.
_Static_assert(POWERMATE_HTTP_MAX_OPEN_SOCKETS + 3 <= CONFIG_LWIP_MAX_SOCKETS,
               "CONFIG_LWIP_MAX_SOCKETS too small for POWERMATE_HTTP_MAX_OPEN_SOCKETS: delete sdkconfig and rebuild");

void start_webserver(void)
{
    auth_init();

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Measured peak is ~3.5 KB (settings and current-limit writes with NVS and I2C,
    // Wi-Fi scan, WebSocket input, ten sessions under a full-rate UART stream).
    config.stack_size = 1024 * 6;
    config.max_uri_handlers = 12;
    config.task_priority = 12;
    config.max_open_sockets = POWERMATE_HTTP_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 15;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    if (httpd_start(&server, &config) != ESP_OK)
    {
        return;
    }

    // Index page
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &index);

    // Login endpoint
    httpd_uri_t login = {.uri = "/login", .method = HTTP_POST, .handler = login_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &login);

    register_wifi_endpoint(server);
    register_ws_endpoint(server);
    register_control_endpoint(server);
    register_diagnostics_endpoint(server);
    register_reboot_endpoint(server);
    register_version_endpoint(server);

    init_status_monitor();

    initialize_dbg_console();
}
