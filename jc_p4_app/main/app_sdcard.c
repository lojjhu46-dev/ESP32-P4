#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "board_pins.h"
#include "app_status.h"
#include "app_sdcard.h"

#define TAG "sdcard"
#define MOUNT_POINT "/sdcard"

/* TF-card power is switched by AO3401 fed from internal LDO_VO4 (3.3V) */
static esp_err_t sd_power_on(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 4,               /* ESP_LDO_VO4 */
        .voltage_mv = 3300,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &ldo);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LDO VO4 acquire failed (%s), assuming card power always-on", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(10));  /* let the rail settle */
    return ESP_OK;
}

esp_err_t app_sdcard_init(void)
{
    sd_power_on();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* will drop automatically if not stable */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;
    slot.d1  = PIN_SD_D1;
    slot.d2  = PIN_SD_D2;
    slot.d3  = PIN_SD_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (no card inserted?)", esp_err_to_name(err));
        app_status_set(APP_MOD_SD, false, "no card");
        return err;
    }

    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "TF card mounted at %s", MOUNT_POINT);
    app_status_set(APP_MOD_SD, true, "%.0fMB %s",
                   (float)card->csd.capacity / (1024 * 1024),
                   card->cid.name);
    return ESP_OK;
}
