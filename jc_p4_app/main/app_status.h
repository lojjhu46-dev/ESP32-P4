#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Module ids shown on the status page */
typedef enum {
    APP_MOD_I2C = 0,
    APP_MOD_LCD,
    APP_MOD_TOUCH,
    APP_MOD_WIFI,
    APP_MOD_SD,
    APP_MOD_ETH,
    APP_MOD_RS485,
    APP_MOD_AUDIO,
    APP_MOD_BATT,
    APP_MOD_CAM,
    APP_MOD_USB,
    APP_MOD_PIPE,
    APP_MOD_FACE,
    APP_MOD_STREAM,
    APP_MOD_BLE,
    APP_MOD_MAX
} app_module_t;

typedef struct {
    bool ok;                /* module up */
    bool battery_mode;      /* value is battery percent (special rendering) */
    int  value;             /* generic numeric payload (e.g. AP count / battery %) */
    char text[64];
} app_mod_status_t;

/* Thread-safe status store (uses an internal mutex) */
void app_status_set(app_module_t mod, bool ok, const char *fmt, ...);
const app_mod_status_t *app_status_get(app_module_t mod);
const char *app_module_name(app_module_t mod);

#ifdef __cplusplus
}
#endif
