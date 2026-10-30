/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "auth.h"

#include <esp_http_server.h>
#include <esp_random.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "AUTH";

typedef struct
{
    char token[TOKEN_LENGTH];
    bool active;
    time_t creation_time;
} auth_token_t;

static auth_token_t s_tokens[MAX_TOKENS];
static SemaphoreHandle_t s_token_mutex;

void auth_init(void)
{
    s_token_mutex = xSemaphoreCreateMutex();
    if (s_token_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create token mutex");
        return;
    }
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        s_tokens[i].active = false;
        s_tokens[i].token[0] = '\0';
    }
    ESP_LOGI(TAG, "Auth module initialized.");
}

char* auth_generate_token(void)
{
    if (xSemaphoreTake(s_token_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take token mutex");
        return NULL;
    }

    int free_slot = -1;
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (!s_tokens[i].active)
        {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1)
    {
        ESP_LOGW(TAG, "No free token slots available. Invalidating oldest token.");
        time_t oldest_time = time(NULL);
        int oldest_idx = -1;
        for (int i = 0; i < MAX_TOKENS; i++)
        {
            if (s_tokens[i].active && s_tokens[i].creation_time < oldest_time)
            {
                oldest_time = s_tokens[i].creation_time;
                oldest_idx = i;
            }
        }
        if (oldest_idx != -1)
        {
            s_tokens[oldest_idx].active = false;
            free_slot = oldest_idx;
            ESP_LOGI(TAG, "Oldest token at index %d invalidated.", oldest_idx);
        }
        else
        {
            ESP_LOGE(TAG, "Could not find an oldest token to invalidate. This should not happen if all are active.");
            xSemaphoreGive(s_token_mutex);
            return NULL;
        }
    }

    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char* new_token = (char*)malloc(TOKEN_LENGTH);
    if (new_token == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for new token");
        xSemaphoreGive(s_token_mutex);
        return NULL;
    }

    for (int i = 0; i < TOKEN_LENGTH - 1; i++)
    {
        new_token[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    new_token[TOKEN_LENGTH - 1] = '\0';

    strncpy(s_tokens[free_slot].token, new_token, TOKEN_LENGTH);
    s_tokens[free_slot].active = true;
    s_tokens[free_slot].creation_time = time(NULL);

    ESP_LOGI(TAG, "Generated new token at slot %d: %.4s****", free_slot, new_token);

    xSemaphoreGive(s_token_mutex);
    return new_token;
}

bool auth_validate_token(const char* token)
{
    if (token == NULL)
    {
        return false;
    }

    if (xSemaphoreTake(s_token_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take token mutex");
        return false;
    }

    bool valid = false;
    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (s_tokens[i].active && strcmp(s_tokens[i].token, token) == 0)
        {
            valid = true;
            break;
        }
    }

    xSemaphoreGive(s_token_mutex);
    return valid;
}

void auth_invalidate_token(const char* token)
{
    if (token == NULL)
    {
        return;
    }

    if (xSemaphoreTake(s_token_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take token mutex");
        return;
    }

    for (int i = 0; i < MAX_TOKENS; i++)
    {
        if (s_tokens[i].active && strcmp(s_tokens[i].token, token) == 0)
        {
            s_tokens[i].active = false;
            s_tokens[i].token[0] = '\0'; // Clear token string
            ESP_LOGI(TAG, "Token at slot %d invalidated.", i);
            break;
        }
    }

    xSemaphoreGive(s_token_mutex);
}

void auth_cleanup_expired_tokens(void) { ESP_LOGD(TAG, "auth_cleanup_expired_tokens called (no-op for now)."); }

static const char* get_token_from_header(httpd_req_t* req)
{
    char* auth_header = NULL;
    size_t buf_len;

    if (httpd_req_get_hdr_value_len(req, "Authorization") == 0)
    {
        return NULL;
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    auth_header = (char*)malloc(buf_len);
    if (auth_header == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for auth header");
        return NULL;
    }

    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, buf_len) != ESP_OK)
    {
        free(auth_header);
        return NULL;
    }

    if (strncmp(auth_header, "Bearer ", 7) == 0)
    {
        const char* token = auth_header + 7;
        return token;
    }

    free(auth_header);
    return NULL;
}

esp_err_t api_auth_check(httpd_req_t* req)
{
    const char* token = get_token_from_header(req);

    if (token == NULL)
    {
        ESP_LOGW(TAG, "API access attempt without token for URI: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Authorization token required");
        return ESP_FAIL;
    }

    if (!auth_validate_token(token))
    {
        ESP_LOGW(TAG, "API access attempt with invalid token for URI: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid or expired token");
        free((void*)token - 7);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Token validated for URI: %s", req->uri);
    free((void*)token - 7);
    return ESP_OK;
}
