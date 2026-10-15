/* BLE 外设（HANDOVER 任务17，2026-10-04）
 *
 * 拓扑：NimBLE host 跑在 P4（IDF bt 组件，CONFIG_BT_CONTROLLER_DISABLED=y），
 * BT controller 跑在 C6 从机上（esp_hosted_bt_controller_init/enable 走 SDIO
 * RPC），HCI 数据经 esp-hosted 的 VHCI 传输 —— 与 WiFi 共用同一条 C6 链路。
 *
 * 代码移植自 esp_hosted 2.12.13 例程 host_nimble_bleprph_host_only_vhci：
 * - 去掉 console/CLI（那是 passkey 手工输入用的；本工程 IO_CAP_NO_IO = Just Works）；
 * - 去掉扩展广播（P4 不是 NimBLE controller，例程在 P4 上默认也走 legacy 广播）；
 * - demo GATT 服务保留（1 字节特征：读/写/notify + 描述符），手机 nRF Connect
 *   即可验证广播/连接/读写/订阅全链路。
 *
 * ⚠️ 必须在 app_wifi_init() 之后调用（esp_hosted SDIO transport 就绪后才能
 *    对 C6 做 BT controller RPC）。 */
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "esp_log.h"
#include "esp_hosted_misc.h"

/* NimBLE host */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "modlog/modlog.h"

#include "app_status.h"
#include "app_ble.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define TAG "ble"
#define BLE_DEV_NAME "JC-P4"

static volatile bool s_synced;

/* 启动诊断（2026-10-04 踩坑产物）：main 组件链接在最后 -> 本构造器在
 * esp_hosted 构造器之后、调度器启动（idle 任务创建，其栈从内部堆分配 1536B）
 * 之前执行。本工程内部 RAM 在启动窗口非常紧张（曾因 BT 使能多占 ~2KB 而把
 * 空闲任务栈挤出堆 -> 无限重启），这行日志是内部堆水位的第一手观测点。 */
static void __attribute__((constructor)) ble_pre_sched_heap_probe(void)
{
    ESP_LOGI(TAG, "pre-sched heap: internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

/* demo GATT 服务（例程原样）：可订阅特征 + 自定义描述符 */
#define GATT_SVR_SVC_ALERT_UUID 0x1811

static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0x2d, 0x71, 0xa2, 0x59, 0xb4, 0x58, 0xc8, 0x12,
                     0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);

static uint8_t gatt_svr_chr_val;
static uint16_t gatt_svr_chr_val_handle;
static const ble_uuid128_t gatt_svr_chr_uuid =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11,
                     0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33);

static uint8_t gatt_svr_dsc_val;
static const ble_uuid128_t gatt_svr_dsc_uuid =
    BLE_UUID128_INIT(0x01, 0x01, 0x01, 0x01, 0x12, 0x12, 0x12, 0x12,
                     0x23, 0x23, 0x23, 0x23, 0x34, 0x34, 0x34, 0x34);

static uint8_t own_addr_type;
static int bleprph_gap_event(struct ble_gap_event *event, void *arg);

/* IDF nimble port 的 store 配置（bond/CCC 记录），例程同款前置声明 */
void ble_store_config_init(void);

static void log_addr(const uint8_t *addr)
{
    ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static int
gatt_svr_write(struct os_mbuf *om, uint16_t min_len, uint16_t max_len,
               void *dst, uint16_t *len)
{
    uint16_t om_len;
    int rc;

    om_len = OS_MBUF_PKTLEN(om);
    if (om_len < min_len || om_len > max_len) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    rc = ble_hs_mbuf_to_flat(om, dst, max_len, len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return 0;
}

static int
gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (attr_handle == gatt_svr_chr_val_handle) {
            rc = os_mbuf_append(ctxt->om, &gatt_svr_chr_val,
                                sizeof(gatt_svr_chr_val));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        goto unknown;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (attr_handle == gatt_svr_chr_val_handle) {
            rc = gatt_svr_write(ctxt->om, sizeof(gatt_svr_chr_val),
                                sizeof(gatt_svr_chr_val),
                                &gatt_svr_chr_val, NULL);
            /* 写入后向所有已订阅的对端发 notify（例程同款演示） */
            ble_gatts_chr_updated(attr_handle);
            ESP_LOGI(TAG, "chr write %u, notify scheduled", gatt_svr_chr_val);
            return rc;
        }
        goto unknown;

    case BLE_GATT_ACCESS_OP_READ_DSC:
        if (ble_uuid_cmp(ctxt->dsc->uuid, &gatt_svr_dsc_uuid.u) == 0) {
            rc = os_mbuf_append(ctxt->om, &gatt_svr_dsc_val,
                                sizeof(gatt_svr_dsc_val));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        goto unknown;

    default:
        goto unknown;
    }

unknown:
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        { {
                .uuid = &gatt_svr_chr_uuid.u,
                .access_cb = gatt_svc_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
                .val_handle = &gatt_svr_chr_val_handle,
                .descriptors = (struct ble_gatt_dsc_def[])
                { {
                      .uuid = &gatt_svr_dsc_uuid.u,
                      .att_flags = BLE_ATT_F_READ,
                      .access_cb = gatt_svc_access,
                  }, {
                      0,
                  }
                },
            }, {
                0,
            }
        },
    },
    {
        0,
    },
};

static int
gatt_svr_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    gatt_svr_dsc_val = 0x99;
    return 0;
}

static void
gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(DEBUG, "registered service %s handle=%d",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(DEBUG, "registering chr %s def=%d val=%d",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;
    default:
        break;
    }
}

/* 通用发现广播：flags + TX power + 完整设备名 + Alert 服务 UUID */
static void
bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(GATT_SVR_SVC_ALERT_UUID)
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as '%s'", name);
    app_status_set(APP_MOD_BLE, true, "adv '%s'", name);
}

static int
bleprph_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "peer:"); log_addr(desc.peer_id_addr.val);
                app_status_set(APP_MOD_BLE, true, "conn handle=%d",
                               event->connect.conn_handle);
            }
        } else {
            /* 连接失败；重新广播 */
            bleprph_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
        /* 连接断开；恢复广播 */
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete; reason=%d", event->adv_complete.reason);
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe; conn=%d attr=%d notify=%d indicate=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update; conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* 已有 bond 但对端重新配对：丢弃旧 bond 重试（例程同款取舍） */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

static void
bleprph_on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset; reason=%d", reason);
}

static void
bleprph_on_sync(void)
{
    s_synced = true;
    int rc;

    /* 公共地址（P4 侧无 controller，地址来自 C6 的公共地址） */
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "addr type infer failed rc=%d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "device address:"); log_addr(addr_val);
    }

    bleprph_advertise();
}

static void
ble_host_task(void *param)
{
    /* nimble_port_stop() 被调用才返回 */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void
ble_sync_watchdog(void *arg)
{
    if (!s_synced) {
        /* HCI 从未同步：多见于老从机固件 BT 通路不可用 / controller 未起来 */
        ESP_LOGE(TAG, "no HCI sync in 8s - BLE radio dead (check C6 slave fw "
                      "BT support); host stack stays up");
        app_status_set(APP_MOD_BLE, false, "no hci sync");
    }
}

void app_ble_init(void)
{
    int rc;

    app_status_set(APP_MOD_BLE, false, "starting");

    /* C6 侧 BT controller（RPC over SDIO）。⚠️ 出厂从机固件 2.3.x 不认识新版
     * feature-control RPC（FEATURE_BT_INIT/ENABLE），会返回失败——但旧版从机的
     * BT controller 本来就是自启的（"disabled by default, host must enable" 是
     * 新版从机才有的行为），其能力位图明确广播了 "HCI over SDIO / BLE only"。
     * 所以这里失败不致命：继续初始化 NimBLE host，HCI 通路是否真正可用由
     * sync 回调（bleprph_on_sync -> 开始广播）裁决；老固件上 RPC 若能成功
     * 则照常走 init/enable。 */
    if (esp_hosted_bt_controller_init() != ESP_OK ||
        esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGW(TAG, "C6 bt controller RPC failed (old slave fw) - assuming "
                      "slave enables its own BT controller, continuing");
    } else {
        ESP_LOGI(TAG, "C6 bt controller enabled");
    }

    rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed %d", rc);
        app_status_set(APP_MOD_BLE, false, "nimble fail");
        return;
    }

    /* Just Works（无 IO）：不需要 console/passkey 输入 */
    ble_hs_cfg.reset_cb = bleprph_on_reset;
    ble_hs_cfg.sync_cb = bleprph_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_sc = 0;

    rc = gatt_svr_init();
    assert(rc == 0);

    rc = ble_svc_gap_device_name_set(BLE_DEV_NAME);
    assert(rc == 0);

    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "nimble host started (name '%s')", BLE_DEV_NAME);

    /* 8s 内没等到 HCI sync 就给出明确信号（老从机固件上 BT 通路可能不通） */
    const esp_timer_create_args_t wd_args = {
        .callback = ble_sync_watchdog, .name = "ble-sync-wd",
    };
    esp_timer_handle_t wd;
    if (esp_timer_create(&wd_args, &wd) == ESP_OK) {
        esp_timer_start_once(wd, 8 * 1000000ULL);
    }
}
