#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the 10.1" 800x1280 MIPI-DSI panel (JD9365, 2 lanes),
 * backlight (I2C 0x45 reg 0x96) and GT911 touch, then start LVGL
 * and build the module status page.
 *
 * Must be called AFTER app_i2c_init().
 */
esp_err_t app_display_init(void);

/* Set backlight brightness 0..100 (%), thread-safe via I2C bus */
esp_err_t app_display_backlight_set(uint8_t percent);

/* Register a battery-percent provider so the status page can live-update.
 * Pass NULL to disable. */
void app_display_set_battery_provider(int (*provider)(void));

#ifdef __cplusplus
}
#endif
