//
// Created by shinys on 25. 8. 18..
//

#ifndef ODROID_REMOTE_HTTP_WEBSERVER_H
#define ODROID_REMOTE_HTTP_WEBSERVER_H

#include "esp_http_server.h"
#include <stddef.h>
#include <stdint.h>

void register_wifi_endpoint(httpd_handle_t server);
void register_ws_endpoint(httpd_handle_t server);
void register_control_endpoint(httpd_handle_t server);
void push_data_to_ws(const uint8_t* data, size_t len);
void register_reboot_endpoint(httpd_handle_t server);
esp_err_t change_baud_rate(int baud_rate);
void register_version_endpoint(httpd_handle_t server);

#endif // ODROID_REMOTE_HTTP_WEBSERVER_H
