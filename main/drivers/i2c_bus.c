/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "i2c_bus.h"
#include <string.h>
#include "sdkconfig.h"

#define I2C_BUS_PORT 0
// A transfer is a few bytes; this only bounds a stuck bus.
#define I2C_BUS_TIMEOUT_MS 50
#define I2C_BUS_MAX_WRITE 4

static i2c_master_bus_handle_t s_bus;

esp_err_t i2c_bus_init(void)
{
    if (s_bus)
        return ESP_OK;

    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = CONFIG_I2C_GPIO_SDA,
        .scl_io_num = CONFIG_I2C_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The board has external pull-ups.
        .flags.enable_internal_pullup = false,
    };
    return i2c_new_master_bus(&config, &s_bus);
}

esp_err_t i2c_bus_add_device(uint8_t addr, uint32_t scl_hz, i2c_master_dev_handle_t* dev)
{
    if (!s_bus || !dev)
        return ESP_ERR_INVALID_STATE;

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = scl_hz,
    };
    return i2c_master_bus_add_device(s_bus, &config, dev);
}

esp_err_t i2c_bus_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t* data, size_t len)
{
    if (len > I2C_BUS_MAX_WRITE)
        return ESP_ERR_INVALID_SIZE;

    uint8_t buf[1 + I2C_BUS_MAX_WRITE];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(dev, buf, len + 1, I2C_BUS_TIMEOUT_MS);
}

esp_err_t i2c_bus_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, I2C_BUS_TIMEOUT_MS);
}
