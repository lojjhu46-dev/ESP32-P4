/*
 * app_netlink.c —— JCLINK over TCP（以太网）
 */
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"

#include "app_netlink.h"
#include "app_usb_link.h"
#include "app_status.h"

#define TAG "netlink"

#define RX_CHUNK        2048
#define NETLINK_STACK   4096
#define NETLINK_PRIO    4

static int s_listen = -1;
static volatile int s_client = -1;
static SemaphoreHandle_t s_tx_lock;
static uint8_t *s_rx_buf;

/* ---- jcl_transport_t 后端：往 TCP 客户端写 ---- */

static esp_err_t tcp_write(const uint8_t *data, uint32_t len)
{
    int fd = s_client;
    if (fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint32_t off = 0;
    esp_err_t err = ESP_OK;
    while (off < len) {
        int n = send(fd, data + off, len - off, 0);
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        off += (uint32_t)n;
    }
    xSemaphoreGive(s_tx_lock);
    return err;
}

static esp_err_t tcp_flush(void)
{
    return ESP_OK;      /* TCP 无 CDC 那种显式 flush */
}

static const jcl_transport_t s_tcp_tp = {
    .name = "tcp",
    .write = tcp_write,
    .flush = tcp_flush,
};

/* ---- 连接处理 ---- */

static void netlink_serve(int fd, const char *peer_ip)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    s_client = fd;
    app_usb_link_set_transport(&s_tcp_tp);
    ESP_LOGI(TAG, "client %s connected -> JCLINK over TCP", peer_ip);
    app_status_set(APP_MOD_ETH, true, "tcp %s", peer_ip);
    app_usb_link_log("netlink: client %s connected, JCLINK over TCP (port %d)",
                     peer_ip, CONFIG_APP_NETLINK_PORT);

    while (1) {
        int n = recv(fd, s_rx_buf, RX_CHUNK, 0);
        if (n > 0) {
            app_usb_link_feed(s_rx_buf, (uint32_t)n);
            continue;
        }
        if (n == 0) {
            ESP_LOGW(TAG, "client %s closed the connection", peer_ip);
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        ESP_LOGW(TAG, "recv error %d on client %s", errno, peer_ip);
        break;
    }

    app_usb_link_set_transport(NULL);       /* 回落到 USB CDC */
    app_usb_link_rx_reset();                /* 丢弃可能残留的半截帧 */
    s_client = -1;
    close(fd);
    app_status_set(APP_MOD_ETH, true, "tcp idle");
    ESP_LOGI(TAG, "client %s gone, tx backend back to USB CDC", peer_ip);
}

static void netlink_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(:%d) failed: errno %d", port, errno);
        close(s_listen);
        s_listen = -1;
        vTaskDelete(NULL);
        return;
    }
    if (listen(s_listen, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno %d", errno);
        close(s_listen);
        s_listen = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "JCLINK over TCP listening on :%d (one client at a time)", port);
    app_status_set(APP_MOD_ETH, true, "tcp :%d", port);

    while (1) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(s_listen, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        char ip[16] = "?";
        inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
        netlink_serve(fd, ip);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t app_netlink_init(uint16_t port)
{
    ESP_RETURN_ON_FALSE(port > 0, ESP_ERR_INVALID_ARG, TAG, "bad port");
    if (s_tx_lock) {
        return ESP_OK;      /* already up */
    }
    ESP_RETURN_ON_FALSE(app_usb_link_ready(), ESP_ERR_INVALID_STATE, TAG, "usb_link not inited");

    s_tx_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tx_lock, ESP_ERR_NO_MEM, TAG, "tx lock");
    s_rx_buf = heap_caps_malloc(RX_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_rx_buf, ESP_ERR_NO_MEM, TAG, "rx buf");

    BaseType_t ok = xTaskCreate(netlink_task, "netlink", NETLINK_STACK,
                                (void *)(uintptr_t)port, NETLINK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task create");
    return ESP_OK;
}

bool app_netlink_client_connected(void)
{
    return s_client >= 0;
}
