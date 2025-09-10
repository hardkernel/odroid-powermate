//
// Created by shinys on 25. 9. 4..
//

#ifndef ODROID_POWER_MATE_CLIMIT_H
#define ODROID_POWER_MATE_CLIMIT_H

#include "esp_err.h"

#include <stdbool.h>

#define VIN_CURRENT_LIMIT_MAX 8.0f
#define MAIN_CURRENT_LIMIT_MAX 7.5f
#define USB_CURRENT_LIMIT_MAX 4.5f

esp_err_t climit_set_vin(double value);
esp_err_t climit_set_main(double value);
esp_err_t climit_set_usb(double value);
bool is_overcurrent();

#endif // ODROID_POWER_MATE_CLIMIT_H
