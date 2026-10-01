#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Init I2C bus 1 (SDA=GPIO7 SCL=GPIO8). Must be first: display/audio/camera SCCB share it. */
esp_err_t app_i2c_init(void);
i2c_master_bus_handle_t app_i2c_get_bus(void);

/* 扫描 0x08..0x77 并打印应答的地址（板级诊断） */
void app_i2c_scan(void);

#ifdef __cplusplus
}
#endif
