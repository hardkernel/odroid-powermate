/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "dbg_console.h"

#include <stdio.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi.h"


/* 'wifi_scan' command */
static int wifi_scan_handler(int argc, char** argv)
{
    printf("Scanning for Wi-Fi networks...\n");

    wifi_ap_record_t* ap_records;
    uint16_t count = 0;

    wifi_scan_aps(&ap_records, &count);

    if (count == 0)
    {
        printf("No APs found.\n");
        return 0;
    }

    printf("Found %d APs:\n", count);
    printf("  %-32s %-4s %s\n", "SSID", "RSSI", "Auth Mode");
    for (int i = 0; i < count; i++)
    {
        printf("  %-32s %-4d %s\n", ap_records[i].ssid, ap_records[i].rssi, auth_mode_str(ap_records[i].authmode));
    }

    if (count > 0)
    {
        free(ap_records);
    }

    return 0;
}

static void register_wifi_scan(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi_scan",
        .help = "Scan for available Wi-Fi networks",
        .hint = NULL,
        .func = &wifi_scan_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static struct
{
    struct arg_str* ssid;
    struct arg_str* password;
    struct arg_end* end;
} wifi_connect_args;

static int wifi_connect_handler(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&wifi_connect_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, wifi_connect_args.end, argv[0]);
        return 1;
    }

    const char* ssid = wifi_connect_args.ssid->sval[0];
    char password[64] = "";

    if (wifi_connect_args.password->count != 0)
        strncpy(password, wifi_connect_args.password->sval[0], sizeof(password));

    printf("Attempting to connect to SSID: %s\n", ssid);

    esp_err_t err = wifi_sta_set_ap(ssid, password);

    if (err == ESP_OK)
    {
        printf("Wi-Fi credentials set. The device will attempt to connect.\n");
    }
    else
    {
        printf("Failed to set Wi-Fi credentials.\n");
    }

    return 0;
}

static void register_wifi_connect(void)
{
    wifi_connect_args.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID of the network to connect to");
    wifi_connect_args.password = arg_str0(NULL, NULL, "<password>", "Password of the network");
    wifi_connect_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {.command = "wifi_connect",
                                   .help = "Connect to a Wi-Fi network",
                                   .hint = NULL,
                                   .func = &wifi_connect_handler,
                                   .argtable = &wifi_connect_args};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* 'wifi_status' command */
static int wifi_status_handler(int argc, char** argv)
{
    wifi_ap_record_t ap_info;
    esp_err_t err = wifi_get_current_ap_info(&ap_info);

    if (err == ESP_OK)
    {
        printf("Connected to AP:\n");
        printf("  SSID: %s\n", (char*)ap_info.ssid);
        printf("  RSSI: %d\n", ap_info.rssi);

        esp_netif_ip_info_t ip_info;
        err = wifi_get_current_ip_info(&ip_info);

        if (err == ESP_OK)
        {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            printf("  IP Address: %s\n", ip_str);

            esp_ip4addr_ntoa(&ip_info.gw, ip_str, sizeof(ip_str));
            printf("  Gateway: %s\n", ip_str);

            esp_ip4addr_ntoa(&ip_info.netmask, ip_str, sizeof(ip_str));
            printf("  Subnet Mask: %s\n", ip_str);
        }
        else
        {
            printf("  Could not get IP information: %s\n", esp_err_to_name(err));
        }
    }
    else
    {
        printf("Not connected to any AP.\n");
    }

    return 0;
}

static void register_wifi_status(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi_status",
        .help = "Get current Wi-Fi connection status and IP information",
        .hint = NULL,
        .func = &wifi_status_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

esp_err_t initialize_dbg_console(void)
{
    esp_console_repl_t* repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    repl_config.prompt = "powermate >";
    repl_config.max_cmdline_length = 512;

    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

    esp_console_register_help_command();
    register_wifi_scan();
    register_wifi_connect();
    register_wifi_status();

    printf("Debug console initialized.\n");

    esp_console_start_repl(repl);

    return ESP_OK;
}
