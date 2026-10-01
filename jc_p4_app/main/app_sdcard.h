#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount TF card (SDMMC slot1, 4-bit, power via internal LDO VO4) at /sdcard */
esp_err_t app_sdcard_init(void);

#ifdef __cplusplus
}
#endif
