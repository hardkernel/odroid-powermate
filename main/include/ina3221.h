/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef POWERMATE_INA3221_H
#define POWERMATE_INA3221_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// TI INA3221 triple-channel shunt and bus voltage monitor. Channels are 0-based: CH1 = 0.
#define INA3221_CHANNELS 3

/** Alert flag bit of a channel in ina3221_status_t, the chip's order: CH1 = bit 2, CH3 = bit 0. */
#define INA3221_FLAG_BIT(ch) (1u << (2 - (ch)))

/** Samples averaged into each result: 1, 4, 16, 64, 128, 256, 512 or 1024. */
typedef enum
{
    INA3221_AVG_1,
    INA3221_AVG_4,
    INA3221_AVG_16,
    INA3221_AVG_64,
    INA3221_AVG_128,
    INA3221_AVG_256,
    INA3221_AVG_512,
    INA3221_AVG_1024,
} ina3221_avg_t;

/** Time of one conversion. */
typedef enum
{
    INA3221_CT_140US,
    INA3221_CT_204US,
    INA3221_CT_332US,
    INA3221_CT_588US,
    INA3221_CT_1100US,
    INA3221_CT_2116US,
    INA3221_CT_4156US,
    INA3221_CT_8244US,
} ina3221_ct_t;

/**
 * Continuous shunt and bus conversions on all channels. The channels share one ADC and are
 * converted in turn. The warning alert compares averaged results; the critical alert
 * compares every single shunt conversion.
 */
typedef struct
{
    ina3221_avg_t avg;
    ina3221_ct_t bus_ct;
    ina3221_ct_t shunt_ct;
    bool latch_critical; ///< Hold the critical alert and its flags until the status is read.
    bool latch_warning;  ///< Hold the warning alert and its flags until the status is read.
} ina3221_config_t;

typedef enum
{
    INA3221_LIMIT_WARNING,
    INA3221_LIMIT_CRITICAL,
} ina3221_limit_t;

typedef struct
{
    uint8_t critical;      ///< Critical alert flags, INA3221_FLAG_BIT() per channel.
    uint8_t warning;       ///< Warning alert flags, INA3221_FLAG_BIT() per channel.
    bool conversion_ready; ///< A new averaged result of every channel since the last status read.
} ina3221_status_t;

/** Attaches the INA3221 at @p addr and applies @p config. */
esp_err_t ina3221_init(uint8_t addr, const ina3221_config_t* config);

/** Time between averaged results of one channel (all channels converted once per sample). */
uint32_t ina3221_cycle_us(const ina3221_config_t* config);

/** Reads the status. This clears the latched alert flags and the conversion-ready flag. */
esp_err_t ina3221_read_status(ina3221_status_t* status);

/** Latest result of one channel: bus voltage in volts, shunt voltage in microvolts. */
esp_err_t ina3221_read_channel(uint8_t ch, float* bus_v, float* shunt_uv);

/** Sets an alert limit on a channel's shunt voltage, in microvolts (40 uV steps). */
esp_err_t ina3221_set_limit(uint8_t ch, ina3221_limit_t limit, float shunt_uv);

/** Reads back an alert limit, in microvolts, and optionally its raw register value. */
esp_err_t ina3221_get_limit(uint8_t ch, ina3221_limit_t limit, float* shunt_uv, uint16_t* raw);

#endif // POWERMATE_INA3221_H
