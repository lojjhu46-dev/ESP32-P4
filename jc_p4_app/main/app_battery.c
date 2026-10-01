#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "board_pins.h"
#include "app_status.h"
#include "app_battery.h"

#define TAG "battery"

#define V_C_MAX   2500      /* mV, full charge */
#define V_C_MIN   2250      /* mV, empty */
#define SAMPLES   200

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static volatile int s_percent = -1;

static bool cali_init(void)
{
    adc_cali_handle_t handle = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_2,
        .chan = ADC_CHANNEL_4,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, &handle) == ESP_OK) {
        s_cali = handle;
        return true;
    }
#endif
    return false;
}

static void battery_task(void *arg)
{
    int raw = 0, mv = 0;
    while (1) {
        int avg = 0;
        for (int i = 0; i < SAMPLES; i++) {
            ESP_ERROR_CHECK(adc_oneshot_read(s_adc, ADC_CHANNEL_4, &raw));
            avg += raw;
        }
        avg /= SAMPLES;

        if (s_cali && adc_cali_raw_to_voltage(s_cali, avg, &mv) == ESP_OK) {
            int pct = (mv - V_C_MIN) * 100 / (V_C_MAX - V_C_MIN);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            /* simple IIR smoothing */
            s_percent = (s_percent < 0) ? pct : ((s_percent + pct) / 2);
            ESP_LOGD(TAG, "raw=%d mv=%d pct=%d", avg, mv, s_percent);
            app_status_set(APP_MOD_BATT, true, "%dmV", mv);
        } else {
            app_status_set(APP_MOD_BATT, true, "raw %d", avg);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t app_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_4, &chan_cfg));

    bool calibrated = cali_init();
    ESP_LOGI(TAG, "battery ADC ready (calibrated=%d)", calibrated);

    if (xTaskCreate(battery_task, "battery", 3072, NULL, 3, NULL) != pdPASS) {
        app_status_set(APP_MOD_BATT, false, "task fail");
        return ESP_FAIL;
    }
    return ESP_OK;
}

int app_battery_get_percent(void)
{
    return s_percent;
}