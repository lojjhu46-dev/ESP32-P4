#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "board_pins.h"
#include "app_i2c.h"
#include "app_status.h"
#include "app_audio.h"

#define TAG "audio"

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_MUTE_GPIO       (-1)      /* PA handled by es8311 driver pa_pin */

static esp_codec_dev_handle_t s_spk = NULL;
static esp_codec_dev_handle_t s_mic = NULL;

static esp_err_t audio_i2s_init(i2s_chan_handle_t *out_tx, i2s_chan_handle_t *out_rx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, out_tx, out_rx), TAG, "i2s new channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_SCLK,
            .ws   = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT,
            .din  = PIN_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*out_tx, &std_cfg), TAG, "init std tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*out_rx, &std_cfg), TAG, "init std rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*out_tx), TAG, "enable tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*out_rx), TAG, "enable rx");
    return ESP_OK;
}

esp_err_t app_audio_init(void)
{
    /* 1. I2S standard bus with MCLK */
    i2s_chan_handle_t tx_handle = NULL, rx_handle = NULL;
    esp_err_t err = audio_i2s_init(&tx_handle, &rx_handle);
    if (err != ESP_OK) {
        app_status_set(APP_MOD_AUDIO, false, "i2s fail");
        return err;
    }

    /* 2. Shared codec data interface */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = tx_handle,
        .rx_handle = rx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        app_status_set(APP_MOD_AUDIO, false, "data_if fail");
        return ESP_FAIL;
    }

    /* 3. ES8311: DAC/speaker out (I2C addr default 0x18) */
    audio_codec_i2c_cfg_t es8311_i2c_cfg = {
        .port = APP_I2C_PORT,
        .bus_handle = app_i2c_get_bus(),
        .addr = 0x18,
    };
    const audio_codec_ctrl_if_t *es8311_ctrl = audio_codec_new_i2c_ctrl(&es8311_i2c_cfg);
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = es8311_ctrl,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_PA_CTRL,          /* NS4150 amp enable, driven by codec dev */
        .use_mclk = true,
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_cfg);

    /* 4. ES7210: mic array ADC in (I2C addr default 0x40) */
    audio_codec_i2c_cfg_t es7210_i2c_cfg = {
        .port = APP_I2C_PORT,
        .bus_handle = app_i2c_get_bus(),
        .addr = 0x40,
    };
    const audio_codec_ctrl_if_t *es7210_ctrl = audio_codec_new_i2c_ctrl(&es7210_i2c_cfg);
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = es7210_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *es7210 = es7210_codec_new(&es7210_cfg);
    if (!es8311 || !es7210) {
        ESP_LOGE(TAG, "codec driver create failed (check I2C)");
        app_status_set(APP_MOD_AUDIO, false, "codec fail");
        return ESP_FAIL;
    }

    /* 5. codec device handles */
    esp_codec_dev_cfg_t dev_out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311,
        .data_if = data_if,
    };
    s_spk = esp_codec_dev_new(&dev_out_cfg);

    esp_codec_dev_cfg_t dev_in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210,
        .data_if = data_if,
    };
    s_mic = esp_codec_dev_new(&dev_in_cfg);
    if (!s_spk || !s_mic) {
        app_status_set(APP_MOD_AUDIO, false, "dev fail");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_spk, &fs), TAG, "open spk");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_spk, 60), TAG, "set vol");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic, &fs), TAG, "open mic");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_mic, 30.0), TAG, "set mic gain");

    ESP_LOGI(TAG, "audio ready: ES8311 out + ES7210 in @ %d Hz", AUDIO_SAMPLE_RATE);
    app_status_set(APP_MOD_AUDIO, true, "ES8311+ES7210 @16k");

    app_audio_beep(1000, 150);
    return ESP_OK;
}

void app_audio_beep(uint32_t freq_hz, uint32_t ms)
{
    if (!s_spk) return;
    const int frames = AUDIO_SAMPLE_RATE * ms / 1000;
    static int16_t *buf = NULL;
    if (!buf) {
        buf = heap_caps_malloc(AUDIO_SAMPLE_RATE * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
        if (!buf) return;
    }
    int remain = frames;
    while (remain > 0) {
        int n = remain > AUDIO_SAMPLE_RATE ? AUDIO_SAMPLE_RATE : remain;
        for (int i = 0; i < n; i++) {
            /* few ms of fade to avoid pop */
            float t = (float)(frames - remain + i) / AUDIO_SAMPLE_RATE;
            float fade = (t < 0.01f) ? (t / 0.01f) : ((frames - remain - i) < 40 ? 1.0f : 1.0f);
            int16_t s = (int16_t)(8000 * fade * sinf(2 * 3.1415926f * freq_hz * t));
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
        }
        size_t bytes = n * 2 * sizeof(int16_t);
        if (esp_codec_dev_write(s_spk, buf, bytes) != ESP_OK) break;
        remain -= n;
    }
}
