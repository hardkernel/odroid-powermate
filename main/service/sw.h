//
// Created by vl011 on 2025-08-28.
//

#ifndef ODROID_POWER_MATE_SW_H
#define ODROID_POWER_MATE_SW_H
#include "esp_err.h"
#include <stdbool.h>

void init_sw();
void config_sw();
void publish_load_switch_status();
esp_err_t sync_load_switch_status_from_hw();
void trig_power();
void trig_reset();
esp_err_t set_load_switches(bool main_on, bool usb_on);
esp_err_t set_main_load_switch(bool on);
esp_err_t set_usb_load_switch(bool on);
bool get_main_load_switch();
bool get_usb_load_switch();
bool get_restore_output_state();
esp_err_t set_restore_output_state(bool enabled);
esp_err_t persist_load_switch_state();

#endif // ODROID_POWER_MATE_SW_H
