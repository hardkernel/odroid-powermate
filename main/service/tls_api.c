/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

// /api/tls: HTTPS on/off and the certificate it serves.
//   GET                                  -> {https_enabled, https_active, certificate:{...}}
//   POST {"https_enabled": true|false}   -> stored, the device reboots into the new mode
//   POST {"action": "generate"}          -> new self-signed EC P-256 certificate
//   POST {"cert": "<PEM>", "key": "<PEM>"} -> user certificate chain and private key
// Certificate changes take effect at the next boot. The private key is never returned.

#include <stdlib.h>
#include <string.h>
#include "auth.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "event.h"
#include "nconfig.h"
#include "system.h"
#include "tls_cert.h"
#include "webserver.h"

static const char* TAG = "tls_api";

// A chain, a key and the JSON around them.
#define TLS_API_MAX_BODY (TLS_CERT_MAX_PEM + TLS_KEY_MAX_PEM + 1024)

static bool https_enabled_setting(void)
{
    char buf[8] = "";
    return nconfig_read(HTTPS_ENABLED, buf, sizeof(buf)) == ESP_OK && strcmp(buf, "true") == 0;
}

static esp_err_t send_json(httpd_req_t* req, cJSON* root)
{
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body)
        return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static cJSON* status_object(void)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "https_enabled", https_enabled_setting());
    cJSON_AddBoolToObject(root, "https_active", webserver_is_https());
    cJSON* cert = cJSON_AddObjectToObject(root, "certificate");
    tls_cert_describe(cert);
    return root;
}

static esp_err_t tls_get_handler(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
        return err;
    return send_json(req, status_object());
}

static char* read_body(httpd_req_t* req)
{
    if (req->content_len == 0 || req->content_len > TLS_API_MAX_BODY)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body missing or too large");
        return NULL;
    }
    char* buf = malloc(req->content_len + 1);
    if (!buf)
    {
        httpd_resp_send_500(req);
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len)
    {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
            continue;
        if (r <= 0)
        {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    return buf;
}

static void wipe_free(char* p)
{
    if (p)
    {
        memset(p, 0, strlen(p));
        free(p);
    }
}

static esp_err_t tls_post_handler(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
        return err;

    char* body = read_body(req);
    if (!body)
        return ESP_FAIL;
    cJSON* root = cJSON_Parse(body);
    wipe_free(body); // may hold a private key
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* enable = cJSON_GetObjectItem(root, "https_enabled");
    cJSON* action = cJSON_GetObjectItem(root, "action");
    cJSON* cert = cJSON_GetObjectItem(root, "cert");
    cJSON* key = cJSON_GetObjectItem(root, "key");
    const char* error = NULL;
    bool reboot = false;

    if (cJSON_IsBool(enable))
    {
        bool on = cJSON_IsTrue(enable);
        if (on && !tls_cert_ready())
            error = "No usable certificate; generate or upload one first";
        else if (on != https_enabled_setting())
        {
            if (nconfig_write(HTTPS_ENABLED, on ? "true" : "false") != ESP_OK)
                error = "Saving the setting failed";
            else
            {
                ESP_LOGI(TAG, "HTTPS %s, rebooting", on ? "enabled" : "disabled");
                push_eventf(EV_INFO, "HTTPS %s; rebooting", on ? "enabled" : "disabled");
                reboot = true;
            }
        }
    }
    else if (cJSON_IsString(action) && strcmp(action->valuestring, "generate") == 0)
    {
        if (tls_cert_generate() != ESP_OK)
            error = "Creating the certificate failed";
        else
            push_eventf(EV_INFO, "new self-signed HTTPS certificate created");
    }
    else if (cJSON_IsString(cert) && cJSON_IsString(key))
    {
        const char* reason = NULL;
        if (tls_cert_install(cert->valuestring, key->valuestring, &reason) != ESP_OK)
            error = reason ? reason : "Installing the certificate failed";
        else
            push_eventf(EV_INFO, "HTTPS certificate installed");
    }
    else
    {
        error = "Expected https_enabled, action \"generate\", or cert and key";
    }

    if (cJSON_IsString(key))
        memset(key->valuestring, 0, strlen(key->valuestring));
    cJSON_Delete(root);

    if (error)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        return ESP_FAIL;
    }

    cJSON* resp = status_object();
    // A new certificate is served from the next boot; the mode switch reboots now.
    cJSON_AddBoolToObject(resp, "reboot", reboot);
    cJSON_AddBoolToObject(resp, "restart_required", !reboot && webserver_is_https());
    err = send_json(req, resp);
    if (reboot)
        start_reboot_timer(3);
    return err;
}

esp_err_t register_tls_endpoint(httpd_handle_t server)
{
    httpd_uri_t get = {.uri = "/api/tls", .method = HTTP_GET, .handler = tls_get_handler};
    httpd_uri_t post = {.uri = "/api/tls", .method = HTTP_POST, .handler = tls_post_handler};
    esp_err_t err = httpd_register_uri_handler(server, &get);
    if (err == ESP_OK)
        err = httpd_register_uri_handler(server, &post);
    return err;
}
