#include "auth.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sw.h"
#include "webserver.h"

static esp_err_t control_get_handler(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
    {
        return err;
    }

    cJSON* root = cJSON_CreateObject();

    cJSON_AddBoolToObject(root, "load_12v_on", get_main_load_switch());
    cJSON_AddBoolToObject(root, "load_5v_on", get_usb_load_switch());

    char* json_string = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));

    free(json_string);
    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t control_post_handler(httpd_req_t* req)
{
    esp_err_t err = api_auth_check(req);
    if (err != ESP_OK)
    {
        return err;
    }

    char buf[128];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0)
    {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        return ESP_FAIL;
    }

    cJSON* item_12v = cJSON_GetObjectItem(root, "load_12v_on");
    if (cJSON_IsBool(item_12v))
        set_main_load_switch(cJSON_IsTrue(item_12v));

    cJSON* item_5v = cJSON_GetObjectItem(root, "load_5v_on");
    if (cJSON_IsBool(item_5v))
        set_usb_load_switch(cJSON_IsTrue(item_5v));

    cJSON* power_trigger = cJSON_GetObjectItem(root, "power_trigger");
    if (cJSON_IsTrue(power_trigger))
        trig_power();

    cJSON* reset_trigger = cJSON_GetObjectItem(root, "reset_trigger");
    if (cJSON_IsTrue(reset_trigger))
        trig_reset();

    cJSON_Delete(root);

    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

void register_control_endpoint(httpd_handle_t server)
{
    init_sw();
    httpd_uri_t get_uri = {.uri = "/api/control", .method = HTTP_GET, .handler = control_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &get_uri);

    httpd_uri_t post_uri = {
        .uri = "/api/control", .method = HTTP_POST, .handler = control_post_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &post_uri);
}
