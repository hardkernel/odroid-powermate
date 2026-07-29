//
// Created by shinys on 25. 9. 1..
//

#ifndef ODROID_POWER_MATE_PRIV_WIFI_H
#define ODROID_POWER_MATE_PRIV_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

void wifi_init_sta(void);
void wifi_init_ap(void);
void initialize_sntp(void);
void wifi_set_auto_reconnect(bool enable);
bool wifi_get_auto_reconnect(void);
bool wifi_sta_is_connected(void);
void wifi_control_lock(void);
void wifi_control_unlock(void);
esp_err_t wifi_connect_locked(void);
void wifi_cancel_sta_connection(void);
void wifi_mark_sta_credentials_pending_validation(void);
void wifi_schedule_reconnect(void);
void wifi_reset_reconnect_backoff(void);
void wifi_prepare_sta_disconnect(void);
bool wifi_wait_for_sta_disconnect(uint32_t timeout_ms);

#endif // ODROID_POWER_MATE_PRIV_WIFI_H
