/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "webserver.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
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
#include "sw.h"
#include "tls_cert.h"
#include "event.h"
#include "wifi.h"
#include "esp_https_server.h"

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
// Same for the HTTPS settings: without dynamic buffers six sessions would need ~180 KB,
// and the TLS 1.3 server crashed with them.
#if !CONFIG_MBEDTLS_DYNAMIC_BUFFER || CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 || !CONFIG_MBEDTLS_X509_CRT_WRITE_C || \
    !CONFIG_ESP_TLS_SERVER_SESSION_TICKETS
#error "sdkconfig predates the HTTPS settings in sdkconfig.defaults: delete sdkconfig and rebuild"
#endif

static bool s_https;

// With HTTPS on, port 80 answers with a redirect to https://. There is no redirect the
// other way: a TLS listener on 443 in HTTP mode made browsers that try HTTPS first stop at
// the self-signed certificate instead of falling back to http://.
// One session: a redirect is answered at once, and LRU purge frees it for the next.
#define REDIRECT_MAX_OPEN_SOCKETS 1

// Every server uses its sessions plus a listening, a control and a message socket.
_Static_assert(POWERMATE_HTTPS_MAX_OPEN_SOCKETS + 3 + REDIRECT_MAX_OPEN_SOCKETS + 3 <= CONFIG_LWIP_MAX_SOCKETS,
               "CONFIG_LWIP_MAX_SOCKETS too small for the HTTPS and redirect servers");

static esp_err_t redirect_handler(httpd_req_t* req)
{
    const char* scheme = req->user_ctx;
    char host[64] = "";
    bool ok = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK;
    char* port = strchr(host, ':');
    if (port)
        *port = '\0';
    // Only a plain name or address goes into the Location header.
    for (const char* c = host; ok && *c; c++)
        ok = isalnum((unsigned char)*c) || *c == '.' || *c == '-';
    if (!ok || !host[0])
        snprintf(host, sizeof(host), "%s.local", wifi_hostname());

    size_t len = strlen(scheme) + 3 + strlen(host) + strlen(req->uri) + 1;
    char* location = malloc(len);
    if (!location)
        return httpd_resp_send_500(req);
    snprintf(location, len, "%s://%s%s", scheme, host, req->uri);
    // Temporary: a browser keeps a 301 for good, which would break switching back.
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, NULL, 0);
    free(location);
    return err;
}

static void start_redirect_to_https(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 2; // +1 is the HTTPS server's
    config.max_open_sockets = REDIRECT_MAX_OPEN_SOCKETS;
    config.max_uri_handlers = 1;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.task_priority = 5; // below the UART bridge
    config.stack_size = 4096;

    esp_err_t err = httpd_start(&server, &config);
    if (err == ESP_OK)
    {
        httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = "https"};
        err = httpd_register_uri_handler(server, &any);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "redirect to https:// not available: %s", esp_err_to_name(err));
}

bool webserver_is_https(void)
{
    return s_https;
}

static bool https_requested(void)
{
    char buf[8] = "";
    return nconfig_read(HTTPS_ENABLED, buf, sizeof(buf)) == ESP_OK && strcmp(buf, "true") == 0;
}

static esp_err_t start_https(httpd_handle_t* server, const httpd_config_t* base)
{
    char *cert = NULL, *key = NULL;
    size_t cert_len, key_len;
    esp_err_t err = tls_cert_ready() ? tls_cert_load(&cert, &cert_len, &key, &key_len) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK)
        return err;

    httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
    uint16_t ctrl_port = ssl.httpd.ctrl_port;
    ssl.httpd = *base;
    ssl.httpd.ctrl_port = ctrl_port;
    ssl.httpd.max_open_sockets = POWERMATE_HTTPS_MAX_OPEN_SOCKETS;
    // The handshakes run here: measured peak ~3.9 KB.
    ssl.httpd.stack_size = 1024 * 8;
    ssl.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
    ssl.port_secure = 443;
    ssl.session_tickets = true;
    // A handshake runs in the httpd task, which also sends every WebSocket frame: a
    // client that connects and never sends a ClientHello must not hold it for the
    // default 10 s. The deadline is checked after each blocking read (5 s), so such a
    // client still holds httpd for ~6 s. A full handshake takes ~0.7 s.
    ssl.tls_handshake_timeout_ms = 3000;
    ssl.servercert = (const uint8_t*)cert;
    ssl.servercert_len = cert_len;
    ssl.prvtkey_pem = (const uint8_t*)key;
    ssl.prvtkey_len = key_len;
    err = httpd_ssl_start(server, &ssl); // keeps its own copies
    memset(key, 0, key_len);
    free(key);
    free(cert);
    return err;
}

void start_webserver(void)
{
    auth_init();

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Measured peak is ~3.5 KB (settings and current-limit writes with NVS and I2C,
    // Wi-Fi scan, WebSocket input, ten sessions under a full-rate UART stream).
    config.stack_size = 1024 * 6;
    config.max_uri_handlers = 16;
    config.task_priority = 12;
    config.max_open_sockets = POWERMATE_HTTP_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 15;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    // HTTPS when it is turned on and the certificate loads, otherwise HTTP: a broken
    // certificate must not leave the device without its web UI.
    if (tls_cert_init() != ESP_OK)
        ESP_LOGE(TAG, "HTTPS certificate store unavailable");
    if (https_requested())
    {
        esp_err_t err = start_https(&server, &config);
        s_https = err == ESP_OK;
        if (!s_https)
        {
            ESP_LOGE(TAG, "HTTPS failed to start (%s), serving HTTP", esp_err_to_name(err));
            push_eventf(EV_WARNING, "HTTPS failed to start (%s), serving HTTP", esp_err_to_name(err));
        }
    }
    if (!s_https && httpd_start(&server, &config) != ESP_OK)
    {
        // Keep the current limits and the debug console even without a web server.
        ESP_LOGE(TAG, "web server failed to start");
        init_sw(); // before the warning task can cut an output
        init_status_monitor();
        initialize_dbg_console();
        return;
    }
    discovery_set_web(s_https);
    if (s_https)
        start_redirect_to_https();

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
    if (register_tls_endpoint(server) != ESP_OK)
        ESP_LOGE(TAG, "TLS endpoint not registered");

    init_status_monitor();

    initialize_dbg_console();
}
