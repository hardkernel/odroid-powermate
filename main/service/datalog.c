#include "datalog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char* TAG = "DATALOG";
static const char* LOG_FILE_PATH = "/littlefs/datalog.csv";
#define MAX_LOG_SIZE (512 * 1024)

void datalog_init(void)
{
    ESP_LOGI(TAG, "Initializing DataLog with LittleFS");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(NULL, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    // Check if file exists
    FILE* f = fopen(LOG_FILE_PATH, "r");
    if (f == NULL)
    {
        ESP_LOGI(TAG, "Log file not found, creating new one.");
        FILE* f_write = fopen(LOG_FILE_PATH, "w");
        if (f_write == NULL)
        {
            ESP_LOGE(TAG, "Failed to create log file.");
        }
        else
        {
            // Add header
            fprintf(f_write, "timestamp,voltage,current,power\n");
            fclose(f_write);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Log file found.");
        fclose(f);
    }
}

void datalog_add(uint32_t timestamp, float voltage, float current, float power)
{
    struct stat st;
    if (stat(LOG_FILE_PATH, &st) == 0)
    {
        if (st.st_size >= MAX_LOG_SIZE)
        {
            ESP_LOGI(TAG, "Log file size (%ld) >= MAX_LOG_SIZE (%d). Truncating.", st.st_size, MAX_LOG_SIZE);

            const char* temp_path = "/littlefs/datalog.tmp";
            FILE* f_read = fopen(LOG_FILE_PATH, "r");
            FILE* f_write = fopen(temp_path, "w");

            if (f_read == NULL || f_write == NULL)
            {
                ESP_LOGE(TAG, "Failed to open files for truncation.");
                if (f_read) fclose(f_read);
                if (f_write) fclose(f_write);
                return;
            }

            char line[256];
            // Copy header
            if (fgets(line, sizeof(line), f_read) != NULL)
            {
                fputs(line, f_write);
            }

            // Skip the oldest data line
            if (fgets(line, sizeof(line), f_read) == NULL)
            {
                // No data lines to skip, something is wrong if we are truncating
            }

            // Copy the rest of the lines
            while (fgets(line, sizeof(line), f_read) != NULL)
            {
                fputs(line, f_write);
            }

            fclose(f_read);
            fclose(f_write);

            // Replace the old log with the new one
            if (remove(LOG_FILE_PATH) != 0)
            {
                ESP_LOGE(TAG, "Failed to remove old log file.");
            }
            if (rename(temp_path, LOG_FILE_PATH) != 0)
            {
                ESP_LOGE(TAG, "Failed to rename temp file.");
                return;
            }
        }
    }

    FILE* f = fopen(LOG_FILE_PATH, "a");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open log file for appending.");
        return;
    }

    fprintf(f, "%lu,%.3f,%.3f,%.3f\n", timestamp, voltage, current, power);
    fclose(f);
}

const char* datalog_get_path(void)
{
    return LOG_FILE_PATH;
}
