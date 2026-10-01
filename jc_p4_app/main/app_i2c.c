#include <stdio.h>
#include "esp_log.h"
#include "board_pins.h"
#include "app_i2c.h"
#include "app_status.h"

static const char *TAG = "i2c";
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t app_i2c_init(void)
{
    if (s_bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = APP_I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* 诊断用：临时打开内部上拉（约 45k）。若原来只有内部上拉在起作用，说明板上外部
           上拉缺失/偏弱；若外部 2.2k~5.1k 正常，打开它没有副作用。 */
        .flags.enable_internal_pullup = 1,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C%d ready: SDA=%d SCL=%d", APP_I2C_PORT, PIN_I2C_SDA, PIN_I2C_SCL);
        app_status_set(APP_MOD_I2C, true, "GPIO7/8 @ bus1");
    } else {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        app_status_set(APP_MOD_I2C, false, "init fail");
    }
    return err;
}

/* 扫一遍总线，看看挂在上面的器件有没有应答（诊断用：GT911=0x5D, ES8311=0x18,
   ES7210=0x40, 背光=0x45, 摄像头 SCCB=0x30/0x36 ...） */
void app_i2c_scan(void)
{
    if (!s_bus) return;
    char found[160];
    size_t off = 0;
    int cnt = 0;
    found[0] = 0;
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) != ESP_OK) continue;
        cnt++;
        int w = snprintf(found + off, sizeof(found) - off, "%s0x%02X", off ? " " : "", addr);
        if (w > 0 && (size_t)w < sizeof(found) - off) off += (size_t)w;
    }
    ESP_LOGW(TAG, "scan: %d device(s) answered on I2C%d: %s", cnt, APP_I2C_PORT, found);
    app_status_set(APP_MOD_I2C, true, "%d dev: %s", cnt, found);
}

i2c_master_bus_handle_t app_i2c_get_bus(void)
{
    return s_bus;
}
