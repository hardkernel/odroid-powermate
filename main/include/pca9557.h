/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef POWERMATE_PCA9557_H
#define POWERMATE_PCA9557_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** Attaches the PCA9557 I/O expander at @p addr. i2c_bus_init() must have run. */
esp_err_t pca9557_init(uint8_t addr);

/** Makes @p pin an output (PCA9557 configuration register bit cleared). */
esp_err_t pca9557_set_output(uint8_t pin);

/** Reads the input port register bit of @p pin (after the chip's polarity inversion). */
esp_err_t pca9557_get_level(uint8_t pin, bool* level);

/** Read-modify-writes the output port register bit of @p pin. Safe to call from several tasks. */
esp_err_t pca9557_set_level(uint8_t pin, bool level);

#endif // POWERMATE_PCA9557_H
