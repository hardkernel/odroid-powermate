//
// Created by shinys on 25. 9. 1..
//

#ifndef ODROID_POWER_MATE_PRIV_WIFI_H
#define ODROID_POWER_MATE_PRIV_WIFI_H

#include <stdbool.h>
#include <stdint.h>

void wifi_init_sta(void);
void wifi_init_ap(void);
void initialize_sntp(void);
void wifi_set_auto_reconnect(bool enable);
bool wifi_get_auto_reconnect(void);
void wifi_prepare_sta_disconnect(void);
bool wifi_wait_for_sta_disconnect(uint32_t timeout_ms);

#endif // ODROID_POWER_MATE_PRIV_WIFI_H
