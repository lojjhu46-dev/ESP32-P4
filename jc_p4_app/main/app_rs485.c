#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"

#include "board_pins.h"
#include "app_status.h"
#include "app_rs485.h"

#define TAG "rs485"
#define BUF_SIZE        256
#define PACKET_TICS     (100 / portTICK_PERIOD_MS)

/* MAX485 on UART1, hardware auto-direction (no DE/RE gpio needed) */
static void rs485_task(void *arg)
{
    uart_config_t cfg = {
        .baud_rate  = RS485_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RS485_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART_NUM, PIN_RS485_TXD, PIN_RS485_RXD,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(RS485_UART_NUM, UART_MODE_RS485_HALF_DUPLEX));
    ESP_ERROR_CHECK(uart_set_rx_timeout(RS485_UART_NUM, 3));

    ESP_LOGI(TAG, "RS485 half-duplex echo ready: UART%d TX=%d RX=%d @%d",
             RS485_UART_NUM, PIN_RS485_TXD, PIN_RS485_RXD, RS485_BAUD);
    app_status_set(APP_MOD_RS485, true, "echo @115200");

    const char heartbeat[] = "JC-P4 RS485 alive";
    uint8_t *data = malloc(BUF_SIZE);
    uint32_t hb = 0;

    while (1) {
        int len = uart_read_bytes(RS485_UART_NUM, data, BUF_SIZE, PACKET_TICS);
        if (len > 0) {
            ESP_LOGI(TAG, "rx %d bytes:", len);
            esp_log_buffer_hex(TAG, data, len);
            uart_write_bytes(RS485_UART_NUM, data, len);   /* echo back */
        }
        if ((++hb % 30) == 0) {                            /* ~3 s heartbeat */
            uart_write_bytes(RS485_UART_NUM, heartbeat, sizeof(heartbeat) - 1);
        }
    }
}

esp_err_t app_rs485_init(void)
{
    if (xTaskCreate(rs485_task, "rs485", 4096, NULL, 5, NULL) != pdPASS) {
        app_status_set(APP_MOD_RS485, false, "task fail");
        return ESP_FAIL;
    }
    return ESP_OK;
}