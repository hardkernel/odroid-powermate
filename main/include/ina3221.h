/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef POWERMATE_INA3221_H
#define POWERMATE_INA3221_H

#include <stdint.h>
#include "esp_err.h"

// Register map (TI INA3221 datasheet, SBOS576). Channels are 0-based: CH1 = 0.
#define INA3221_CHANNELS 3
#define INA3221_REG_CONFIG 0x00
#define INA3221_REG_SHUNT(ch) (0x01 + 2 * (ch))
#define INA3221_REG_BUS(ch) (0x02 + 2 * (ch))
#define INA3221_REG_CRITICAL(ch) (0x07 + 2 * (ch))
#define INA3221_REG_WARNING(ch) (0x08 + 2 * (ch))
#define INA3221_REG_MASK 0x0F

// Configuration register fields.
#define INA3221_CONFIG_CH_EN(ch) (1u << (14 - (ch)))
#define INA3221_CONFIG_AVG(code) ((uint16_t)(code) << 9)
#define INA3221_CONFIG_VBUS_CT(code) ((uint16_t)(code) << 6)
#define INA3221_CONFIG_VSH_CT(code) ((uint16_t)(code) << 3)
#define INA3221_CONFIG_MODE_CONTINUOUS 0x7 // Shunt and bus, continuous

// Averaging count codes: 1, 4, 16, 64, 128, 256, 512, 1024 samples.
enum { INA3221_AVG_1, INA3221_AVG_4, INA3221_AVG_16, INA3221_AVG_64,
       INA3221_AVG_128, INA3221_AVG_256, INA3221_AVG_512, INA3221_AVG_1024 };
// Conversion time codes: 140, 204, 332, 588, 1100, 2116, 4156, 8244 us.
enum { INA3221_CT_140US, INA3221_CT_204US, INA3221_CT_332US, INA3221_CT_588US,
       INA3221_CT_1100US, INA3221_CT_2116US, INA3221_CT_4156US, INA3221_CT_8244US };

// Mask/Enable register. Reading it clears the latched alert flags.
#define INA3221_MASK_CVRF (1u << 0)
#define INA3221_MASK_WEN (1u << 10)
#define INA3221_MASK_CEN (1u << 11)
// Alert flags as 3-bit fields: bit 2 = CH1, bit 1 = CH2, bit 0 = CH3.
#define INA3221_MASK_WF(mask) (((mask) >> 3) & 0x7)
#define INA3221_MASK_CF(mask) (((mask) >> 7) & 0x7)
#define INA3221_FLAG_BIT(ch) (1u << (2 - (ch)))

/** Attaches the INA3221 at @p addr and writes the configuration and Mask/Enable registers. */
esp_err_t ina3221_init(uint8_t addr, uint16_t config, uint16_t mask_enable);

esp_err_t ina3221_read(uint8_t reg, uint16_t* value);
esp_err_t ina3221_write(uint8_t reg, uint16_t value);

/** Bus voltage register to volts (8 mV LSB in bits 15..3). */
static inline float ina3221_bus_volts(uint16_t raw)
{
    return (int16_t)raw * 0.001f;
}

/** Shunt voltage or alert limit register to microvolts (40 uV LSB in bits 15..3). */
static inline float ina3221_shunt_microvolts(uint16_t raw)
{
    return (int16_t)raw * 5.0f;
}

/** Microvolts to a shunt voltage alert limit register value. */
static inline uint16_t ina3221_limit_raw(float microvolts)
{
    return (uint16_t)(int16_t)(microvolts / 5.0f);
}

#endif // POWERMATE_INA3221_H
