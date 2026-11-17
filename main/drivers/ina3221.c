/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "ina3221.h"
#include "i2c_bus.h"

// The INA3221 supports up to 2.44 MHz; 1 MHz (Fast-mode Plus) keeps a register read near 50 us.
#define INA3221_SCL_HZ 1000000

static i2c_master_dev_handle_t s_dev;

esp_err_t ina3221_read(uint8_t reg, uint16_t* value)
{
    uint8_t buf[2];
    esp_err_t err = i2c_bus_read_reg(s_dev, reg, buf, sizeof(buf));
    if (err == ESP_OK)
        *value = (uint16_t)buf[0] << 8 | buf[1]; // MSB first
    return err;
}

esp_err_t ina3221_write(uint8_t reg, uint16_t value)
{
    const uint8_t buf[2] = {value >> 8, value & 0xFF};
    return i2c_bus_write_reg(s_dev, reg, buf, sizeof(buf));
}

esp_err_t ina3221_init(uint8_t addr, uint16_t config, uint16_t mask_enable)
{
    esp_err_t err = s_dev ? ESP_OK : i2c_bus_add_device(addr, INA3221_SCL_HZ, &s_dev);
    if (err == ESP_OK)
        err = ina3221_write(INA3221_REG_CONFIG, config);
    if (err == ESP_OK)
        err = ina3221_write(INA3221_REG_MASK, mask_enable);
    return err;
}
