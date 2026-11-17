/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "pca9557.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"

#define PCA9557_SCL_HZ 400000
#define PCA9557_PINS 8

#define REG_INPUT 0x00
#define REG_OUTPUT 0x01
#define REG_CONFIG 0x03

static i2c_master_dev_handle_t s_dev;
// Bit updates are read-modify-write; the bus driver only serializes single transfers.
static SemaphoreHandle_t s_rmw_lock;

esp_err_t pca9557_init(uint8_t addr)
{
    if (s_dev)
        return ESP_OK;
    s_rmw_lock = xSemaphoreCreateMutex();
    if (!s_rmw_lock)
        return ESP_ERR_NO_MEM;
    return i2c_bus_add_device(addr, PCA9557_SCL_HZ, &s_dev);
}

static esp_err_t update_bit(uint8_t reg, uint8_t pin, bool set)
{
    if (pin >= PCA9557_PINS)
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_rmw_lock, portMAX_DELAY);
    uint8_t value;
    esp_err_t err = i2c_bus_read_reg(s_dev, reg, &value, 1);
    if (err == ESP_OK)
    {
        value = set ? (value | (1u << pin)) : (value & ~(1u << pin));
        err = i2c_bus_write_reg(s_dev, reg, &value, 1);
    }
    xSemaphoreGive(s_rmw_lock);
    return err;
}

esp_err_t pca9557_set_output(uint8_t pin)
{
    return update_bit(REG_CONFIG, pin, false);
}

esp_err_t pca9557_set_level(uint8_t pin, bool level)
{
    return update_bit(REG_OUTPUT, pin, level);
}

esp_err_t pca9557_get_level(uint8_t pin, bool* level)
{
    if (pin >= PCA9557_PINS || !level)
        return ESP_ERR_INVALID_ARG;

    uint8_t value;
    esp_err_t err = i2c_bus_read_reg(s_dev, REG_INPUT, &value, 1);
    if (err == ESP_OK)
        *level = (value >> pin) & 1;
    return err;
}
