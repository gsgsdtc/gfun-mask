/*
 * BLE L2CAP Stub Implementation
 */

#include "esp_log.h"
#include "ble_l2cap_stub.h"

#define TAG "BLE_STUB"

static bool s_initialized = false;

void ble_l2cap_stub_init(void)
{
    s_initialized = true;
    ESP_LOGI(TAG, "BLE L2CAP Stub initialized (no actual BLE)");
}

bool ble_l2cap_is_tx_ready(void)
{
    /* 始终返回就绪，模拟畅通的信道 */
    return true;
}

int ble_l2cap_send_frame(uint8_t type, const uint8_t *data, uint16_t len)
{
    /* 仅打印日志，不实际发送 */
    static int frame_count = 0;
    frame_count++;

    if (frame_count % 50 == 1) {
        ESP_LOGD(TAG, "Frame type=0x%02X, len=%d (count=%d)", type, len, frame_count);
    }

    if (type == 0xFE) {
        ESP_LOGI(TAG, "End of utterance frame (0xFE) would be sent");
    }

    return 0;
}
