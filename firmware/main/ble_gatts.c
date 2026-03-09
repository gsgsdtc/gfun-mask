/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md §2.2
 * @purpose GATT Server（NimBLE）：PSM Service (UUID: AE00) + PSM Characteristic (UUID: AE01, Read)。
 *          iOS 读取 Characteristic 后获得 L2CAP PSM=128（2 字节 Little-Endian）。
 */

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "ble_gatts.h"

#define TAG "BLE_GATTS"

/*
 * PSM Service UUID: 0000AE00-0000-1000-8000-00805F9B34FB
 * NimBLE UUID128 字节序：小端（最低有效字节在前）。
 */
static const ble_uuid128_t s_psm_svc_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x00, 0xae, 0x00, 0x00);

/*
 * PSM Characteristic UUID: 0000AE01-0000-1000-8000-00805F9B34FB
 */
static const ble_uuid128_t s_psm_chr_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x01, 0xae, 0x00, 0x00);

/* PSM 值：128 = 0x80，2 字节 Little-Endian */
static const uint8_t s_psm_value[2] = {
    (uint8_t)(BLE_L2CAP_PSM & 0xFF),
    (uint8_t)((BLE_L2CAP_PSM >> 8) & 0xFF),
};

/* PSM Characteristic 读取回调 */
static int psm_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, s_psm_value, sizeof(s_psm_value));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* NimBLE 静态服务定义表 */
static const struct ble_gatt_svc_def s_gatts_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_psm_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &s_psm_chr_uuid.u,
                .access_cb = psm_chr_access,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            { 0 }, /* 终止符 */
        },
    },
    { 0 }, /* 终止符 */
};

void vm_gatts_init(void)
{
    int rc;

    rc = ble_gatts_count_cfg(s_gatts_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(s_gatts_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "PSM GATT Service registered (PSM=%d)", BLE_L2CAP_PSM);
}
