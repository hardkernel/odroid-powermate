/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef POWERMATE_I2C_BUS_H
#define POWERMATE_I2C_BUS_H

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/** Creates the shared I2C master bus on the CONFIG_I2C_GPIO_SDA/SCL pins. Call once at boot. */
esp_err_t i2c_bus_init(void);

/** Attaches a 7-bit device at @p scl_hz. The bus driver serializes transfers between tasks. */
esp_err_t i2c_bus_add_device(uint8_t addr, uint32_t scl_hz, i2c_master_dev_handle_t* dev);

/** Writes @p len bytes after the register address byte. */
esp_err_t i2c_bus_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t* data, size_t len);

/** Sets the register pointer, then reads @p len bytes (repeated start). */
esp_err_t i2c_bus_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data, size_t len);

#endif // POWERMATE_I2C_BUS_H
