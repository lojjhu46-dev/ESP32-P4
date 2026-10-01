#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board_pins.h"
#include "app_i2c.h"
#include "app_status.h"
#include "app_display.h"
#include "esp_lcd_jd9365_10_1.h"

static const char *TAG = "display";

#define MIPI_DSI_PHY_PWR_LDO_CHAN        3      /* LDO_VO3 -> VDD_MIPI_DPHY */
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV  2500

static lv_obj_t *s_value_label[APP_MOD_MAX];
static lv_obj_t *s_battery_label = NULL;
static int (*s_batt_provider)(void) = NULL;

/* ---------------- backlight via I2C (backlight IC @0x45 reg 0x96) ---------------- */
esp_err_t app_display_backlight_set(uint8_t percent)
{
    i2c_master_bus_handle_t bus = app_i2c_get_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;

    i2c_device_config_t dev_cfg = {
        .device_address = LCD_BACKLIGHT_I2C_ADDR,
        .scl_speed_hz = 100 * 1000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) return err;

    uint8_t tx[2] = { LCD_BACKLIGHT_REG, (uint8_t)(255u * percent / 100u) };
    err = i2c_master_transmit(dev, tx, sizeof(tx), 50 /*ms*/);
    i2c_master_bus_rm_device(dev);
    return err;
}

void app_display_set_battery_provider(int (*provider)(void))
{
    s_batt_provider = provider;
}

/* ---------------- status page UI ---------------- */
static void build_status_page(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00e0a0), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_text(title, "JC-ESP32P4-M3-DEV");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *sub = lv_label_create(scr);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8a9aa8), 0);
    lv_label_set_text(sub, "P4 @360MHz + C6 Wi-Fi6  |  framework self-test");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 60);

    for (int i = 0; i < APP_MOD_MAX; i++) {
        int col = (i % 2) * 330;
        int row = (i / 2) * 84;

        lv_obj_t *name = lv_label_create(scr);
        lv_obj_set_style_text_color(name, lv_color_hex(0xcfd8dc), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
        lv_label_set_text(name, app_module_name((app_module_t)i));
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 40 + col, 140 + row);

        s_value_label[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(s_value_label[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_value_label[i], LV_ALIGN_TOP_LEFT, 40 + col, 172 + row);
        lv_label_set_text(s_value_label[i], "...");
    }

    s_battery_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0xffd166), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_battery_label, "BAT --%");
    lv_obj_align(s_battery_label, LV_ALIGN_BOTTOM_MID, 0, -30);
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    for (int i = 0; i < APP_MOD_MAX; i++) {
        const app_mod_status_t *st = app_status_get((app_module_t)i);
        if (!st || !s_value_label[i]) continue;

        char buf[96];
        lv_color_t color;
        if (st->ok) {
            snprintf(buf, sizeof(buf), "OK  %s", st->text);
            color = lv_color_hex(0x3ddc84);
        } else {
            snprintf(buf, sizeof(buf), "ERR %s", st->text);
            color = lv_color_hex(0xff6b6b);
        }
        lv_label_set_text(s_value_label[i], buf);
        lv_obj_set_style_text_color(s_value_label[i], color, 0);
    }

    if (s_battery_label && s_batt_provider) {
        int pct = s_batt_provider();
        char buf[32];
        snprintf(buf, sizeof(buf), "BAT %d%%", pct);
        lv_label_set_text(s_battery_label, buf);
    }
}

/* ---------------- init sequence ---------------- */
esp_err_t app_display_init(void)
{
    esp_err_t ret = ESP_OK;

    /* 1. Power MIPI DSI PHY from internal LDO VO3 (2.5V) - must come first */
    esp_ldo_channel_handle_t ldo_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_GOTO_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_phy), err, TAG, "acquire LDO VO3 failed");

    /* 2. MIPI-DSI bus: 2 data lanes, 1500 Mbps */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), err, TAG, "new DSI bus failed");

    /* 3. DBI panel IO (for vendor init commands) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io_handle), err, TAG, "new DBI io failed");

    /* 4. DPI panel (800x1280 @60Hz, RGB565) */
    esp_lcd_dpi_panel_config_t dpi_cfg = JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    jd9365_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = 2,
        },
        .flags.use_mipi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,                    /* LCD_RST not wired to GPIO */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_jd9365(io_handle, &panel_cfg, &panel_handle), err, TAG, "new jd9365 panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel_handle), err, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel_handle), err, TAG, "panel init failed");

    /* 5. Backlight on (100%) */
    app_display_backlight_set(100);

    /* 6. GT911 touch on the shared I2C bus */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = 100 * 1000;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(app_i2c_get_bus(), &tp_io_cfg, &tp_io), err, TAG, "touch io failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,                      /* no reset/int pin wired */
        .int_gpio_num = -1,
        .flags.swap_xy = 0,
        .flags.mirror_x = 0,
        .flags.mirror_y = 0,
    };
    esp_lcd_touch_handle_t tp_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp_handle), err, TAG, "gt911 init failed");

    /* 7. LVGL port */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_GOTO_ON_ERROR(lvgl_port_init(&lvgl_cfg), err, TAG, "lvgl port init failed");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .control_handle = NULL,
        .buffer_size = LCD_H_RES * 50,           /* pixels */
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,                 /* big buffers in PSRAM (DPI panel has own FB) */
            .sw_rotate = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_port_cfg = {
        .flags.avoid_tearing = false,
    };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_port_cfg);
    ESP_GOTO_ON_FALSE(disp != NULL, ESP_FAIL, err, TAG, "add lvgl display failed");

    const lvgl_port_touch_cfg_t touch_port_cfg = {
        .disp = disp,
        .handle = tp_handle,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_port_cfg);
    ESP_GOTO_ON_FALSE(indev != NULL, ESP_FAIL, err, TAG, "add lvgl touch failed");

    /* 8. Build the status page inside LVGL lock */
    while (!lvgl_port_lock(0)) { }
    build_status_page();
    lv_timer_create(refresh_timer_cb, 500, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "display ready: JD9365 %dx%d, GT911 touch, backlight 100%%", LCD_H_RES, LCD_V_RES);
    app_status_set(APP_MOD_LCD, true, "JD9365 800x1280");
    app_status_set(APP_MOD_TOUCH, true, "GT911 ready");
    return ESP_OK;

err:
    ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
    app_status_set(APP_MOD_LCD, false, "%s", esp_err_to_name(ret));
    return ret;
}
