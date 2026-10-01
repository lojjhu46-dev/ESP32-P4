#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Battery gauge: GPIO53 (ADC2_CH4), 2250..2500 mV -> 0..100 % */
esp_err_t app_battery_init(void);
int app_battery_get_percent(void);

#ifdef __cplusplus
}
#endif