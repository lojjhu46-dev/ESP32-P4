#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "app_status.h"
#include "app_wifi.h"
#include "sdkconfig.h"

#define TAG "wifi"
#define SCAN_LIST_SIZE 20
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static bool s_have_cfg = false;      /* 已有凭据（Kconfig 或运行时配置）才发起/重试连接 */
static esp_netif_t *s_sta_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_have_cfg) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
            if (s_have_cfg) {
                esp_wifi_connect();   /* retry */
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        app_status_set(APP_MOD_WIFI, true, "ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

esp_err_t app_wifi_init(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    /* ⚠️ 必须创建 STA netif，否则关联成功也拿不到 IP（DHCP 无处运行）。
     * 厂商 wifi_scan demo 只做扫描不需要 netif，所以从未暴露这个缺口。 */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif, ESP_FAIL, TAG, "create sta netif");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));       /* transparently routed to C6 via esp_hosted/SDIO */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));  /* 凭据写入 NVS，断电保存 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* NVS 里有保存的凭据（之前 {"cmd":"wifi"} 配过）则开机自动重连 */
    wifi_config_t stored = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK && stored.sta.ssid[0]) {
        s_have_cfg = true;
    }
    if (strlen(CONFIG_APP_WIFI_SSID)) {
        s_have_cfg = true;
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Blocking scan to prove the C6 link works */
    uint16_t number = SCAN_LIST_SIZE;
    wifi_ap_record_t ap_info[SCAN_LIST_SIZE];
    uint16_t ap_count = 0;
    memset(ap_info, 0, sizeof(ap_info));

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s (check C6 slave firmware / SDIO)", esp_err_to_name(err));
        app_status_set(APP_MOD_WIFI, false, "scan fail");
        return err;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));

    ESP_LOGI(TAG, "=== %u APs found ===", ap_count);
    for (int i = 0; (i < SCAN_LIST_SIZE) && (i < ap_count); i++) {
        ESP_LOGI(TAG, "| %-*s %d", 33, ap_info[i].ssid, ap_info[i].rssi);
    }

    if (s_have_cfg) {
        wifi_config_t wifi_cfg = { 0 };
        strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        app_status_set(APP_MOD_WIFI, true, "connecting...");
        esp_wifi_connect();
        /* got-IP handler updates status */
    } else {
        app_status_set(APP_MOD_WIFI, true, "%u APs", ap_count);
    }
    return ESP_OK;
}

/* 运行时配置 WiFi（{"cmd":"wifi"} 用）：写 NVS（FLASH 存储），断电保存；
 * 立即发起连接，断线由事件处理器自动重试 */
void app_wifi_set_credentials(const char *ssid, const char *pass)
{
    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, ssid ? ssid : "", sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass ? pass : "", sizeof(wifi_cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    s_have_cfg = true;
    esp_wifi_connect();
    app_status_set(APP_MOD_WIFI, true, "connecting %s", ssid ? ssid : "");
}

/* STA 当前 IP（未关联/DHCP 未完成返回 false）；结果帧 "wip" 字段用 */
bool app_wifi_get_ip(char *out, size_t len)
{
    if (!s_sta_netif) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(out, len, IPSTR, IP2STR(&info.ip));
    return true;
}
