#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_status.h"

static app_mod_status_t s_status[APP_MOD_MAX];
static SemaphoreHandle_t s_lock;
static const char *const s_names[APP_MOD_MAX] = {
    "I2C bus", "LCD", "Touch", "Wi-Fi", "TF card",
    "Ethernet", "RS485", "Audio", "Battery", "Camera", "USB link",
    "Image pipe", "Face AI", "Img stream", "BLE",
};

static void ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void app_status_set(app_module_t mod, bool ok, const char *fmt, ...)
{
    if (mod >= APP_MOD_MAX) return;
    ensure_lock();
    va_list args;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status[mod].ok = ok;
    va_start(args, fmt);
    vsnprintf(s_status[mod].text, sizeof(s_status[mod].text), fmt, args);
    va_end(args);
    xSemaphoreGive(s_lock);
}

const app_mod_status_t *app_status_get(app_module_t mod)
{
    if (mod >= APP_MOD_MAX) return NULL;
    ensure_lock();
    return &s_status[mod];
}

const char *app_module_name(app_module_t mod)
{
    if (mod >= APP_MOD_MAX) return "?";
    return s_names[mod];
}
