//
// Created by shinys on 25. 9. 1..
//

#ifndef ODROID_POWER_MATE_PRIV_WIFI_H
#define ODROID_POWER_MATE_PRIV_WIFI_H

void wifi_init_sta(void);
void wifi_init_ap(void);
void initialize_sntp(void);
void wifi_set_auto_reconnect(bool enable);

#endif // ODROID_POWER_MATE_PRIV_WIFI_H
