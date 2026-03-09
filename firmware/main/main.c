/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md
 *          docs/modules/audio-capture/design/01-esp32-audio-capture-design.md
 * @purpose 系统入口：
 *          - 初始化 NVS、启动 NimBLE 协议栈
 *          - 初始化音频采集流水线
 *          - 处理 iOS 控制指令
 */

#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_gap.h"
#include "ble_gatts.h"
#include "ble_l2cap.h"
#include "audio_driver.h"
#include "audio_pipeline.h"

#define TAG "MAIN"

/* ── 控制指令处理 ─────────────────────────────────────────── */

static void handle_l2cap_command(frame_type_t cmd)
{
    switch (cmd) {
    case FRAME_TYPE_CMD_START_RECORD:
        ESP_LOGI(TAG, "Received START_RECORD command");
        if (audio_pipeline_start() != 0) {
            ESP_LOGE(TAG, "Failed to start audio pipeline");
        }
        break;

    case FRAME_TYPE_CMD_STOP_RECORD:
        ESP_LOGI(TAG, "Received STOP_RECORD command");
        if (audio_pipeline_stop() != 0) {
            ESP_LOGE(TAG, "Failed to stop audio pipeline");
        }
        break;

    default:
        ESP_LOGW(TAG, "Unknown command: 0x%02X", cmd);
        break;
    }
}

/* ── NimBLE Host 回调 ─────────────────────────────────────── */

/* NimBLE Host 就绪回调：确定地址类型后开始广播 */
static void on_sync(void)
{
    ble_gap_on_sync();
}

/* NimBLE Host 重置回调（异常恢复） */
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

/* NimBLE Host Task：调用 nimble_port_run() 直到协议栈停止 */
static void host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task started");
    nimble_port_run();                  /* 阻塞直到 nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ── 外部驱动注册（由 audio_driver_es7210.c 提供）─────────── */

extern void audio_driver_es7210_register(void);

/* ── 主入口 ───────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 1. NVS（PHY 校准数据存储）──────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. NimBLE 协议栈初始化 ──────────────────────────── */
    ESP_ERROR_CHECK(nimble_port_init());

    /* ── 3. NimBLE Host 配置 ─────────────────────────────── */
    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_sc           = 0;    /* 不启用 Secure Connections */

    /* ── 4. 注册内置 GAP / GATT 服务 ────────────────────── */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* ── 5. 注册 PSM GATT 服务 ───────────────────────────── */
    vm_gatts_init();

    /* ── 6. 初始化 L2CAP CoC 内存池 ─────────────────────── */
    vm_l2cap_init();

    /* ── 7. 设置控制指令回调 ─────────────────────────────── */
    ble_l2cap_set_cmd_callback(handle_l2cap_command);

    /* ── 8. 初始化音频流水线 ─────────────────────────────── */
    audio_driver_es7210_register();
    if (audio_pipeline_init() != 0) {
        ESP_LOGE(TAG, "Audio pipeline init failed");
    }

    /* ── 9. 设置设备名称 ─────────────────────────────────── */
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set("VoiceMask-01"));

    /* ── 10. 启动 NimBLE Host Task ───────────────────────── */
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "VoiceMask firmware started (Phase 2)");
}
