/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

//
// Created by vl011 on 2025-08-28.
//

#ifndef ODROID_POWER_MATE_SW_H
#define ODROID_POWER_MATE_SW_H
#include "esp_err.h"
#include <stdbool.h>

#define SW_MAIN (1u << 0)
#define SW_USB (1u << 1)

void init_sw();
void config_sw();
void publish_load_switch_status();
esp_err_t sync_load_switch_status_from_hw();
/** Presses the SBC power (or reset) button. ESP_ERR_INVALID_STATE: the last press is still held. */
esp_err_t trig_power();
esp_err_t trig_reset();
/** Switches the outputs in mask (SW_MAIN, SW_USB); the others keep their level. */
esp_err_t set_load_switch_outputs(unsigned mask, bool main_on, bool usb_on);
/** Protective cut-off of the outputs in mask: waits for the locks and always writes. */
esp_err_t cut_load_switch_outputs(unsigned mask);
esp_err_t set_load_switches(bool main_on, bool usb_on);
esp_err_t set_main_load_switch(bool on);
esp_err_t set_usb_load_switch(bool on);
bool get_main_load_switch();
bool get_usb_load_switch();
bool get_restore_output_state();
esp_err_t set_restore_output_state(bool enabled);
esp_err_t persist_load_switch_state();

#endif // ODROID_POWER_MATE_SW_H
