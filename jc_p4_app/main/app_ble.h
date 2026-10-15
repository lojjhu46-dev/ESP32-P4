#pragma once

/* BLE peripheral on the C6 co-processor.
 * NimBLE host runs on the P4 (bt component, controller disabled), the BT
 * controller runs on the C6 slave, HCI goes over the shared SDIO transport
 * (esp-hosted VHCI). Advertisement name "JC-P4". */

void app_ble_init(void);
