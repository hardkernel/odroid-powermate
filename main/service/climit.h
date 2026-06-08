//
// Created by shinys on 25. 9. 4..
//

#ifndef ODROID_POWER_MATE_CLIMIT_H
#define ODROID_POWER_MATE_CLIMIT_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define VIN_CURRENT_LIMIT_MAX 10.0f
#define MAIN_CURRENT_LIMIT_MAX 10.0f
#define USB_CURRENT_LIMIT_MAX 5.9f

#define VIN_CRITICAL_CURRENT_LIMIT_MAX 15.0f
#define MAIN_CRITICAL_CURRENT_LIMIT_MAX 11.0f
#define USB_CRITICAL_CURRENT_LIMIT_MAX 6.0f
#define CRITICAL_CURRENT_LIMIT_MIN 1.0f

esp_err_t climit_set_vin(double value);
esp_err_t climit_set_main(double value);
esp_err_t climit_set_usb(double value);
esp_err_t climit_set_critical_vin(double value);
esp_err_t climit_set_critical_main(double value);
esp_err_t climit_set_critical_usb(double value);
esp_err_t climit_get_vin(float* limit_a, uint16_t* raw);
esp_err_t climit_get_main(float* limit_a, uint16_t* raw);
esp_err_t climit_get_usb(float* limit_a, uint16_t* raw);
esp_err_t climit_get_critical_vin(float* limit_a, uint16_t* raw);
esp_err_t climit_get_critical_main(float* limit_a, uint16_t* raw);
esp_err_t climit_get_critical_usb(float* limit_a, uint16_t* raw);
bool is_overcurrent();

#endif // ODROID_POWER_MATE_CLIMIT_H
