/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char* TAG = "DISCOVERY";

esp_err_t discovery_start(void)
{
    // Answers <hostname>.local on both the STA network and the setup AP, under the same
    // name the PowerMate gives DHCP. The MAC suffix in the hostname keeps the instance
    // names of several PowerMates on one network apart too.
    const char* hostname = wifi_hostname();
    char instance[48];
    snprintf(instance, sizeof(instance), "ODROID PowerMate %s", hostname + sizeof(CONFIG_LWIP_LOCAL_HOSTNAME));

    // _http._tcp is for browsers and Bonjour browsers. _powermate._tcp lets pmtui and the
    // Claude plugin list PowerMates without probing every HTTP device on the network.
    mdns_txt_item_t txt[] = {
        {"path", "/"},
        {"fw", esp_app_get_description()->version},
    };

    esp_err_t err = mdns_init();
    if (err == ESP_OK)
        err = mdns_hostname_set(hostname);
    if (err == ESP_OK)
        err = mdns_instance_name_set(instance);
    if (err == ESP_OK)
        err = mdns_service_add(NULL, "_http", "_tcp", 80, txt, 1);
    if (err == ESP_OK)
        err = mdns_service_add(NULL, "_powermate", "_tcp", 80, txt + 1, 1);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "mDNS unavailable: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "mDNS: http://%s.local/", hostname);
    return ESP_OK;
}
