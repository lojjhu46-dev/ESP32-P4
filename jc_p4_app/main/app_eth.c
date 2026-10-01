#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "esp_event.h"

#include "board_pins.h"
#include "app_status.h"
#include "app_eth.h"

#define TAG "eth"

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint8_t mac[6] = {0};
    esp_eth_handle_t handle = *(esp_eth_handle_t *)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "link up, HW " MACSTR, MAC2STR(mac));
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "link down");
        app_status_set(APP_MOD_ETH, true, "no link");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "started");
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    app_status_set(APP_MOD_ETH, true, "ip " IPSTR, IP2STR(&e->ip_info.ip));
}

/*
 * 直连 PC 时通常没有 DHCP 服务器（PC 用的是静态 IP），所以给 DHCP 4s 机会，
 * 拿不到租约就回落到 CONFIG_APP_ETH_STATIC_IP（默认与 PC 同网段）。
 */
static void eth_ip_task(void *arg)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    const int polls = 4000 / 200;

    esp_netif_dhcpc_start(netif);       /* 已启动时返回 ALREADY_STARTED，忽略 */
    for (int i = 0; i < polls; i++) {
        esp_netif_ip_info_t ip = { 0 };
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "eth ready (DHCP): " IPSTR, IP2STR(&ip.ip));
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    const char *sip = CONFIG_APP_ETH_STATIC_IP;
    if (!sip || !sip[0]) {
        ESP_LOGW(TAG, "no DHCP lease, no static ip configured");
        app_status_set(APP_MOD_ETH, true, "no ip");
        vTaskDelete(NULL);
        return;
    }

    esp_netif_ip_info_t ip = { 0 };
    esp_ip4_addr_t a = { 0 }, m = { 0 };
    esp_netif_str_to_ip4(sip, &a);
    esp_netif_str_to_ip4(CONFIG_APP_ETH_STATIC_NETMASK, &m);
    ip.ip = a;
    ip.netmask = m;
    ip.gw.addr = 0;                     /* 直连场景无网关，别把自己当网关 */
    esp_netif_dhcpc_stop(netif);
    esp_err_t err = esp_netif_set_ip_info(netif, &ip);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "no DHCP lease -> static %s / %s", sip, CONFIG_APP_ETH_STATIC_NETMASK);
        app_status_set(APP_MOD_ETH, true, "ip " IPSTR, IP2STR(&ip.ip));
    } else {
        ESP_LOGE(TAG, "static ip set failed: %s", esp_err_to_name(err));
        app_status_set(APP_MOD_ETH, false, "no ip");
    }
    vTaskDelete(NULL);
}

esp_err_t app_eth_init(void)
{
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_mdc_gpio_num = PIN_EMAC_MDC;
    emac_cfg.smi_mdio_gpio_num = PIN_EMAC_MDIO;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = PIN_RMII_CLK_IN;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = PIN_PHY_RST;
    phy_cfg.autonego_timeout_ms = 200;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "mac/phy create failed");
        app_status_set(APP_MOD_ETH, false, "create fail");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &eth_handle), TAG, "driver install");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    if (!eth_netif || !glue) {
        /* esp_netif_new 返回 NULL 通常意味着 esp_netif_init() 没被调用过 */
        ESP_LOGE(TAG, "netif create failed (esp_netif_init missing?)");
        app_status_set(APP_MOD_ETH, false, "netif fail");
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_netif_attach(eth_netif, glue), TAG, "netif attach");

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_handler, NULL));

    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), TAG, "start");
    ESP_LOGI(TAG, "RMII ethernet started (IP101: MDC=%d MDIO=%d RST=%d)", PIN_EMAC_MDC, PIN_EMAC_MDIO, PIN_PHY_RST);
    app_status_set(APP_MOD_ETH, true, "started");

    xTaskCreate(eth_ip_task, "eth_ip", 3072, eth_netif, 4, NULL);
    return ESP_OK;
}