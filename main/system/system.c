//
// Created by shinys on 25. 8. 5.
//

#include <system.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_system.h"

static const char* TAG = "odroid";

static esp_timer_handle_t reboot_timer_handle = NULL;

static void reboot_timer_callback(void* arg)
{
    ESP_LOGI(TAG, "Rebooting now...");
    esp_restart();
}

void start_reboot_timer(int sec)
{
    if (reboot_timer_handle != NULL)
    {
        ESP_LOGW(TAG, "The reboot timer is already running.");
        return;
    }

    ESP_LOGI(TAG, "Device will reboot in %d seconds.", sec);

    const esp_timer_create_args_t reboot_timer_args = {.callback = &reboot_timer_callback, .name = "reboot-timer"};

    if (esp_timer_create(&reboot_timer_args, &reboot_timer_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create reboot timer.");
        reboot_timer_handle = NULL;
        return;
    }

    if (esp_timer_start_once(reboot_timer_handle, (uint64_t)sec * 1000000) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start reboot timer.");
        esp_timer_delete(reboot_timer_handle);
        reboot_timer_handle = NULL;
        return;
    }
}

static esp_err_t reboot_post_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    const char* resp_str = "{\"status\": \"reboot timer started\"}";
    httpd_resp_send(req, resp_str, strlen(resp_str));

    start_reboot_timer(3);

    return ESP_OK;
}

void stop_reboot_timer()
{
    if (reboot_timer_handle == NULL)
    {
        return;
    }
    esp_timer_stop(reboot_timer_handle);
    esp_timer_delete(reboot_timer_handle);
    reboot_timer_handle = NULL;
    ESP_LOGI(TAG, "Reboot timer stopped.");
}

void register_reboot_endpoint(httpd_handle_t server)
{
    httpd_uri_t post_uri = {
        .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &post_uri);
}

static esp_err_t version_get_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    char buf[100];
    sprintf(buf, "{\"version\": \"%s\"}", VERSION_HASH);
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

void register_version_endpoint(httpd_handle_t server)
{
    httpd_uri_t post_uri = {
        .uri = "/api/version", .method = HTTP_GET, .handler = version_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &post_uri);
}