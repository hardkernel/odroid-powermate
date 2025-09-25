#include "webserver.h"
#include <stdio.h>
#include <string.h>
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
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_set_type(req, "text/html");

    size_t remaining = index_html_size;
    const char* ptr = (const char*)index_html_start;
    while (remaining > 0) {
        size_t to_send = remaining < 2048 ? remaining : 2048;
        if (httpd_resp_send_chunk(req, ptr, to_send) != ESP_OK) {
            ESP_LOGE(TAG, "File sending failed!");
            httpd_resp_send_chunk(req, NULL, 0);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        ptr += to_send;
        remaining -= to_send;
    }

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

    register_wifi_endpoint(server);
    register_ws_endpoint(server);
    register_control_endpoint(server);
    register_reboot_endpoint(server);
    register_version_endpoint(server);

    init_status_monitor();
}
