/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md §2.1 §3.1
 * @purpose BLE 广播（100ms interval，设备名 VoiceMask-01）与连接事件处理（NimBLE）。
 *          连接后通知 L2CAP 模块建立信道；断连后重新广播，保持对 iOS 可见。
 *
 * 广播布局（31 字节限制）：
 *   Advertising data : Flags + Complete Local Name  (≤ 17 bytes)
 *   Scan response    : 128-bit Service UUID (AE00)  (18 bytes)
 */

#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "ble_gap.h"
#include "ble_l2cap.h"

#define TAG "BLE_GAP"

/* 广播间隔：100ms = 160 × 0.625ms */
#define ADV_ITVL_MS  BLE_GAP_ADV_FAST_INTERVAL1_MIN   /* 0x0030 ≈ 30ms (min) */

static uint8_t s_own_addr_type;

/*
 * PSM Service UUID: 0000AE00-0000-1000-8000-00805F9B34FB
 * BLE 空中传输使用完全反转的字节序（小端序）。
 */
static const ble_uuid128_t s_psm_svc_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x00, 0xae, 0x00, 0x00);

/* ── 内部函数 ─────────────────────────────────────────────── */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected, conn_handle=%d",
                     event->connect.conn_handle);
            /* 通知 L2CAP 模块：BLE 已连接，可以建立 CoC 信道 */
            ble_l2cap_on_ble_connect(event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connect failed (status=%d), re-advertising",
                     event->connect.status);
            ble_gap_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d",
                 event->disconnect.reason);
        /* 通知 L2CAP 模块清理状态 */
        ble_l2cap_on_ble_disconnect();
        /* 重新开始广播 */
        ble_gap_start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* 广播超时（此处设为 BLE_HS_FOREVER，不会触发） */
        ble_gap_start_advertising();
        break;

    default:
        break;
    }
    return 0;
}

/* ── 公开接口 ─────────────────────────────────────────────── */

void ble_gap_on_sync(void)
{
    int rc;

    /* 确保有合法的身份地址 */
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* 推断最优广播地址类型（优先公开地址） */
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer addr type: %d", rc);
        return;
    }

    ble_gap_start_advertising();
}

void ble_gap_start_advertising(void)
{
    struct ble_hs_adv_fields fields   = {0};
    struct ble_gap_adv_params params  = {0};
    int rc;

    /* ── Advertising Data：Flags + 128-bit Service UUID ──────────
     * 128-bit UUID 需要 18 字节（2字节头 + 16字节数据）
     * Flags 需要 3 字节
     * 总共 21 字节，符合 31 字节限制
     * 注意：不带设备名，设备名通过 Scan Response 或连接后 GAP 服务获取
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    fields.uuids128            = &s_psm_svc_uuid;
    fields.num_uuids128        = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    /* ── 启动广播 ───────────────────────────────────────────── */
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* 可连接 */
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* 通用可发现 */
    params.itvl_min  = 0x00A0;                  /* 100ms = 160×0.625ms */
    params.itvl_max  = 0x00A0;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising as 'VoiceMask-01'");
    }
}
