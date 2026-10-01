#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "app_status.h"
#include "app_wifi.h"
#include "sdkconfig.h"

#define TAG "wifi"
#define SCAN_LIST_SIZE 20
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (strlen(CONFIG_APP_WIFI_SSID)) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
            if (strlen(CONFIG_APP_WIFI_SSID)) {
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

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));       /* transparently routed to C6 via esp_hosted/SDIO */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
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

    if (strlen(CONFIG_APP_WIFI_SSID)) {
        wifi_config_t wifi_cfg = { 0 };
        strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        app_status_set(APP_MOD_WIFI, true, "connecting...");
        /* got-IP handler updates status */
    } else {
        app_status_set(APP_MOD_WIFI, true, "%u APs", ap_count);
    }
    return ESP_OK;
}
