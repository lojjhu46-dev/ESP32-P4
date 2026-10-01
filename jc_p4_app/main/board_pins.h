#pragma once
/*
 * JC-ESP32P4-M3-DEV board pin map (single source of truth).
 * Derived from vendor schematics 5-Schematic/4_ESP32P4.png etc.
 */
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- I2C bus 1 (GT911 touch / ES8311 / ES7210 / backlight 0x45 / CSI SCCB) ---------------- */
#define PIN_I2C_SDA            GPIO_NUM_7
#define PIN_I2C_SCL            GPIO_NUM_8
#define APP_I2C_PORT           1

/* ---------------- Audio (I2S0, ES8311 DAC -> NS4150 -> speaker; ES7210 mic ADC) ---------------- */
#define PIN_I2S_MCLK           GPIO_NUM_13
#define PIN_I2S_SCLK           GPIO_NUM_12
#define PIN_I2S_LRCK           GPIO_NUM_10
#define PIN_I2S_DOUT           GPIO_NUM_9     /* P4 -> ES8311 DSDIN */
#define PIN_I2S_DIN            GPIO_NUM_48    /* ES7210 ASDOUT -> P4 */
#define PIN_PA_CTRL            GPIO_NUM_11    /* NS4150 power amp enable */

/* ---------------- Ethernet RMII (IP101GRI, 25MHz xtal, 50MHz RMII_CLK on GPIO50 input) ---------------- */
#define PIN_RMII_TXD0          GPIO_NUM_34
#define PIN_RMII_TXD1          GPIO_NUM_35    /* also BOOT strap */
#define PIN_RMII_TXEN          GPIO_NUM_49
#define PIN_RMII_RXD0          GPIO_NUM_29
#define PIN_RMII_RXD1          GPIO_NUM_30
#define PIN_RMII_RXDV          GPIO_NUM_28
#define PIN_RMII_CLK_IN        GPIO_NUM_50
#define PIN_EMAC_MDC           GPIO_NUM_31
#define PIN_EMAC_MDIO          GPIO_NUM_52
#define PIN_PHY_RST            GPIO_NUM_51

/* ---------------- TF card (SDMMC slot1, fixed pin mapping of ESP32-P4) ---------------- */
#define PIN_SD_CLK             GPIO_NUM_43
#define PIN_SD_CMD             GPIO_NUM_44
#define PIN_SD_D0              GPIO_NUM_39
#define PIN_SD_D1              GPIO_NUM_40
#define PIN_SD_D2              GPIO_NUM_41
#define PIN_SD_D3              GPIO_NUM_42

/* ---------------- RS485 (MAX485 on UART1, HW auto-direction) ---------------- */
#define PIN_RS485_TXD          GPIO_NUM_26
#define PIN_RS485_RXD          GPIO_NUM_27
#define RS485_UART_NUM         1
#define RS485_BAUD             115200

/* ---------------- Battery voltage divider -> ADC ---------------- */
#define PIN_BAT_ADC            GPIO_NUM_53    /* ADC2_CH4 */

/* ---------------- P4 <-> C6 (Wi-Fi slave) ---------------- */
#define PIN_C6_SDIO_CMD        GPIO_NUM_19
#define PIN_C6_SDIO_CLK        GPIO_NUM_18
#define PIN_C6_SDIO_D0         GPIO_NUM_14    /* D1=15 D2=16 D3=17 */
#define PIN_C6_RESET           GPIO_NUM_54    /* C6_CHIP_PU, active-high */
#define PIN_C6_BOOT            GPIO_NUM_6
#define PIN_C6_UART_TX         GPIO_NUM_12    /* P4 -> C6 RX */
#define PIN_C6_UART_RX         GPIO_NUM_13    /* C6 TX -> P4 */

/* ---------------- Reserved / straps ---------------- */
#define PIN_TOUCH_INT          GPIO_NUM_20    /* reserved net on schematic */
#define PIN_TOUCH_RST          GPIO_NUM_21    /* reserved */
#define PIN_LCD_PWM            GPIO_NUM_23    /* reserved (10.1" DSI panel backlight is via I2C 0x45) */
#define PIN_BOOT               GPIO_NUM_35

/* ---------------- Display ---------------- */
#define LCD_H_RES              800
#define LCD_V_RES              1280
#define LCD_BACKLIGHT_I2C_ADDR 0x45
#define LCD_BACKLIGHT_REG      0x96

#ifdef __cplusplus
}
#endif
