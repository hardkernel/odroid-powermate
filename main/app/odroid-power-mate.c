#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "i2cdev.h"
#include "indicator.h"
#include "nconfig.h"
#include "nvs_flash.h"
#include "system.h"
#include "wifi.h"
#include "storage.h"

void app_main(void)
{
    printf("\n\n== ODROID POWER-MATE ===\n");
    printf("Version: %s-%s\n\n", VERSION_TAG, VERSION_HASH);

    ESP_ERROR_CHECK(i2cdev_init());
    init_led();
    led_set(LED_BLU, BLINK_TRIPLE);
    led_off(LED_BLU);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    storage_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(init_nconfig());

    wifi_init();
    wifi_connect();
    sync_time();

    start_webserver();
}
