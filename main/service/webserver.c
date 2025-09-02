#include "webserver.h"
#include <stdio.h>
#include <string.h>
#include "datalog.h"
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

static esp_err_t index_handler(httpd_req_t* req)
{
    extern const unsigned char index_html_start[] asm("_binary_index_html_gz_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_gz_end");
    const size_t index_html_size = (index_html_end - index_html_start);

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char*)index_html_start, index_html_size);

    return ESP_OK;
}

static esp_err_t datalog_download_handler(httpd_req_t* req)
{
    const char* filepath = datalog_get_path();
    FILE* f = fopen(filepath, "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open datalog file for reading");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"datalog.csv\"");

    char buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK)
        {
            ESP_LOGE(TAG, "File sending failed!");
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 1024 * 8;
    config.max_uri_handlers = 10;
    config.task_priority = 12;
    config.max_open_sockets = 7;

    if (httpd_start(&server, &config) != ESP_OK)
    {
        return;
    }

    // Index page
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &index);

    httpd_uri_t datalog_uri = {
        .uri = "/datalog.csv", .method = HTTP_GET, .handler = datalog_download_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &datalog_uri);

    register_wifi_endpoint(server);
    register_ws_endpoint(server);
    register_control_endpoint(server);
    init_status_monitor();
}
