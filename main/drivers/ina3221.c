/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "ina3221.h"
#include "i2c_bus.h"

// The INA3221 supports up to 2.44 MHz; 1 MHz (Fast-mode Plus) keeps a register read near 50 us.
#define INA3221_SCL_HZ 1000000

// Register map (TI INA3221 datasheet, SBOS576).
#define REG_CONFIG 0x00
#define REG_SHUNT(ch) (0x01 + 2 * (ch))
#define REG_BUS(ch) (0x02 + 2 * (ch))
#define REG_CRITICAL(ch) (0x07 + 2 * (ch))
#define REG_WARNING(ch) (0x08 + 2 * (ch))
#define REG_MASK 0x0F

// Configuration register fields.
#define CONFIG_CH_EN(ch) (1u << (14 - (ch)))
#define CONFIG_AVG(code) ((uint16_t)(code) << 9)
#define CONFIG_VBUS_CT(code) ((uint16_t)(code) << 6)
#define CONFIG_VSH_CT(code) ((uint16_t)(code) << 3)
#define CONFIG_MODE_CONTINUOUS 0x7 // Shunt and bus, continuous

// Mask/Enable register. Reading it clears the latched alert flags and CVRF.
// D11 is WEN and D10 is CEN (SBOS576, Mask/Enable register), right above CF1-3 in D9-D7.
#define MASK_CVRF (1u << 0)
#define MASK_CEN (1u << 10)
#define MASK_WEN (1u << 11)
#define MASK_WF(mask) (((mask) >> 3) & 0x7) // Bit 2 = CH1 ... bit 0 = CH3, as INA3221_FLAG_BIT
#define MASK_CF(mask) (((mask) >> 7) & 0x7)

// Shunt voltage, bus voltage and limit registers hold a signed value in bits 15..3.
#define SHUNT_UV_PER_LSB 40.0f // 40 uV per bit 3, so 5 uV per raw count
#define BUS_MV_PER_LSB 8.0f    // 8 mV per bit 3, so 1 mV per raw count

static const uint16_t ct_us[] = {140, 204, 332, 588, 1100, 2116, 4156, 8244};
static const uint16_t avg_samples[] = {1, 4, 16, 64, 128, 256, 512, 1024};

static i2c_master_dev_handle_t s_dev;

static esp_err_t ina3221_read(uint8_t reg, uint16_t* value)
{
    uint8_t buf[2];
    esp_err_t err = i2c_bus_read_reg(s_dev, reg, buf, sizeof(buf));
    if (err == ESP_OK)
        *value = (uint16_t)buf[0] << 8 | buf[1]; // MSB first
    return err;
}

static esp_err_t ina3221_write(uint8_t reg, uint16_t value)
{
    const uint8_t buf[2] = {value >> 8, value & 0xFF};
    return i2c_bus_write_reg(s_dev, reg, buf, sizeof(buf));
}

static float raw_to_shunt_uv(uint16_t raw)
{
    return (int16_t)raw * (SHUNT_UV_PER_LSB / 8);
}

esp_err_t ina3221_init(uint8_t addr, const ina3221_config_t* config)
{
    uint16_t reg = CONFIG_CH_EN(0) | CONFIG_CH_EN(1) | CONFIG_CH_EN(2) | CONFIG_AVG(config->avg) |
        CONFIG_VBUS_CT(config->bus_ct) | CONFIG_VSH_CT(config->shunt_ct) | CONFIG_MODE_CONTINUOUS;
    uint16_t mask = (config->latch_critical ? MASK_CEN : 0) | (config->latch_warning ? MASK_WEN : 0);

    esp_err_t err = s_dev ? ESP_OK : i2c_bus_add_device(addr, INA3221_SCL_HZ, &s_dev);
    if (err == ESP_OK)
        err = ina3221_write(REG_CONFIG, reg);
    if (err == ESP_OK)
        err = ina3221_write(REG_MASK, mask);
    return err;
}

uint32_t ina3221_cycle_us(const ina3221_config_t* config)
{
    return INA3221_CHANNELS * (ct_us[config->bus_ct] + ct_us[config->shunt_ct]) * avg_samples[config->avg];
}

esp_err_t ina3221_read_status(ina3221_status_t* status)
{
    uint16_t mask;
    esp_err_t err = ina3221_read(REG_MASK, &mask);
    if (err != ESP_OK)
        return err;
    status->critical = MASK_CF(mask);
    status->warning = MASK_WF(mask);
    status->conversion_ready = (mask & MASK_CVRF) != 0;
    return ESP_OK;
}

esp_err_t ina3221_read_channel(uint8_t ch, float* bus_v, float* shunt_uv)
{
    uint16_t bus, shunt;
    esp_err_t err = ina3221_read(REG_BUS(ch), &bus);
    if (err == ESP_OK)
        err = ina3221_read(REG_SHUNT(ch), &shunt);
    if (err != ESP_OK)
        return err;
    *bus_v = (int16_t)bus * (BUS_MV_PER_LSB / 8) / 1000.0f;
    *shunt_uv = raw_to_shunt_uv(shunt);
    return ESP_OK;
}

esp_err_t ina3221_set_limit(uint8_t ch, ina3221_limit_t limit, float shunt_uv)
{
    uint16_t raw = (uint16_t)(int16_t)(shunt_uv / (SHUNT_UV_PER_LSB / 8));
    return ina3221_write(limit == INA3221_LIMIT_CRITICAL ? REG_CRITICAL(ch) : REG_WARNING(ch), raw);
}

esp_err_t ina3221_get_limit(uint8_t ch, ina3221_limit_t limit, float* shunt_uv, uint16_t* raw)
{
    uint16_t value;
    esp_err_t err = ina3221_read(limit == INA3221_LIMIT_CRITICAL ? REG_CRITICAL(ch) : REG_WARNING(ch), &value);
    if (err != ESP_OK)
        return err;
    if (shunt_uv)
        *shunt_uv = raw_to_shunt_uv(value);
    if (raw)
        *raw = value;
    return ESP_OK;
}
