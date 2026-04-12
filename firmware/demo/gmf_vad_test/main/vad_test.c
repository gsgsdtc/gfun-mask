/*
 * GMF VAD Element Demo
 *
 * Purpose: 测试 VADNet 在 GMF Pipeline 中独立运行，验证句末检测
 *
 * Architecture:
 *   Mic IO ──▶ VAD Element ──▶ PCM Encoder ──▶ BLE
 *                 │
 *                 └──▶ 检测 VAD_END ──▶ 自动停止 Pipeline
 *
 * Expected Log:
 *   [VAD] Speech detected
 *   [VAD] Speech continued...
 *   [VAD] Silence detected (count=1)
 *   [VAD] Silence detected (count=2)
 *   [VAD] Silence detected (count=3) -> VAD_END triggered
 *   [MAIN] Auto-stop triggered after 500ms silence
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"

#include "audio_driver.h"

/* 音频驱动注册函数（声明自 audio_driver_es7210.c）*/
extern void audio_driver_es7210_register(void);
#include "gmf_mic_io.h"
#include "gmf_pcm_enc_el.h"
#include "gmf_vad_el.h"
#include "esp_vad.h"  /* For VAD_MODE_3 */
#include "ble_l2cap_stub.h"

#define TAG "VAD_TEST"

/* ────────────────────────────────────────────────────────────────
 * 全局状态
 * ──────────────────────────────────────────────────────────────── */

static esp_gmf_pool_handle_t    s_pool     = NULL;
static esp_gmf_pipeline_handle_t s_pipeline = NULL;
static esp_gmf_task_handle_t     s_task     = NULL;
static volatile bool             s_running  = false;
static volatile bool             s_vad_triggered_stop = false;

/* ────────────────────────────────────────────────────────────────
 * VAD 回调：句末检测触发自动停止
 * ──────────────────────────────────────────────────────────────── */

static void on_vad_event(vad_event_t event, void *ctx)
{
    switch (event) {
        case VAD_EVENT_SPEECH_START:
            ESP_LOGI(TAG, "=== VAD: Speech START ===");
            break;

        case VAD_EVENT_SPEECH_END:
            ESP_LOGI(TAG, "=== VAD: Speech END (Auto-stop) ===");
            s_vad_triggered_stop = true;
            break;

        case VAD_EVENT_SILENCE:
            ESP_LOGD(TAG, "VAD: Silence detected");
            break;

        default:
            break;
    }
}

/* ────────────────────────────────────────────────────────────────
 * Pipeline 创建/销毁
 * ──────────────────────────────────────────────────────────────── */

static int create_pipeline(void)
{
    /* 创建 Pool */
    esp_gmf_err_t ret = esp_gmf_pool_init(&s_pool);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool_init failed: %d", ret);
        return -1;
    }

    /* 注册 Mic IO */
    const audio_driver_ops_t *drv = audio_driver_get();
    gmf_mic_io_cfg_t mic_cfg = {
        .name   = "mic_io",
        .driver = drv,
    };
    esp_gmf_io_handle_t mic_io = NULL;
    ret = gmf_mic_io_init(&mic_cfg, &mic_io);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "mic_io_init failed: %d", ret);
        goto fail;
    }
    esp_gmf_pool_register_io(s_pool, mic_io, "mic_io");

    /* 注册 VAD Element */
    gmf_vad_el_cfg_t vad_cfg = {
        .mode = VAD_MODE_3,           /* 最敏感模式 */
        .min_speech_ms = 300,         /* 300ms 语音才算开始 */
        .min_silence_ms = 500,        /* 500ms 静音 = 句末 */
        .callback = on_vad_event,
        .callback_ctx = NULL,
    };
    esp_gmf_element_handle_t vad_el = NULL;
    ret = gmf_vad_el_init(&vad_cfg, &vad_el);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "vad_el_init failed: %d", ret);
        goto fail;
    }
    esp_gmf_pool_register_element(s_pool, vad_el, "vad");

    /* 注册 PCM Encoder Element */
    gmf_pcm_enc_el_cfg_t enc_cfg = {0};
    esp_gmf_element_handle_t enc_el = NULL;
    ret = gmf_pcm_enc_el_init(&enc_cfg, &enc_el);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "enc_el_init failed: %d", ret);
        goto fail;
    }
    esp_gmf_pool_register_element(s_pool, enc_el, "pcm_enc");

    /* 创建 Pipeline: mic ──▶ vad ──▶ enc */
    const char *el_names[] = {"vad", "pcm_enc"};
    ret = esp_gmf_pool_new_pipeline(s_pool, "mic_io",
                                     el_names, 2,
                                     NULL, &s_pipeline);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "new_pipeline failed: %d", ret);
        goto fail;
    }

    /* 创建 Task */
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = 8192;
    task_cfg.thread.prio  = 5;
    task_cfg.thread.core  = 1;
    task_cfg.name = "vad_test_task";

    ret = esp_gmf_task_init(&task_cfg, &s_task);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "task_init failed: %d", ret);
        goto fail;
    }

    esp_gmf_pipeline_bind_task(s_pipeline, s_task);
    esp_gmf_pipeline_loading_jobs(s_pipeline);

    ESP_LOGI(TAG, "Pipeline created: mic -> vad -> pcm_enc");
    return 0;

fail:
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
    return -1;
}

static void destroy_pipeline(void)
{
    if (s_task) {
        esp_gmf_task_deinit(s_task);
        s_task = NULL;
    }
    if (s_pipeline) {
        esp_gmf_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
}

/* ────────────────────────────────────────────────────────────────
 * 测试任务：启动 Pipeline 并等待 VAD 触发停止
 * ──────────────────────────────────────────────────────────────── */

static void vad_test_task(void *arg)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "VAD Test Starting...");
    ESP_LOGI(TAG, "Please say something, then stop talking.");
    ESP_LOGI(TAG, "Pipeline will auto-stop after 500ms silence");
    ESP_LOGI(TAG, "========================================");

    /* 创建 Pipeline */
    if (create_pipeline() != 0) {
        ESP_LOGE(TAG, "Failed to create pipeline");
        vTaskDelete(NULL);
        return;
    }

    /* 启动 Pipeline */
    s_running = true;
    s_vad_triggered_stop = false;

    esp_gmf_err_t ret = esp_gmf_pipeline_run(s_pipeline);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pipeline_run failed: %d", ret);
        destroy_pipeline();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Pipeline running... waiting for VAD_END");

    /* 等待 VAD 触发停止 */
    while (s_running && !s_vad_triggered_stop) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_vad_triggered_stop) {
        ESP_LOGI(TAG, "Auto-stopping pipeline due to VAD_END...");
    }

    /* 停止 Pipeline */
    esp_gmf_pipeline_stop(s_pipeline);
    s_running = false;

    ESP_LOGI(TAG, "Pipeline stopped");

    destroy_pipeline();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "VAD Test Complete!");
    ESP_LOGI(TAG, "========================================");

    vTaskDelete(NULL);
}

/* ────────────────────────────────────────────────────────────────
 * 主入口
 * ──────────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 初始化 BLE stub（实际 demo 可能不需要 BLE） */
    ble_l2cap_stub_init();

    /* 注册音频驱动 */
    audio_driver_es7210_register();

    /* 初始化音频硬件 */
    const audio_driver_ops_t *drv = audio_driver_get();
    if (drv->init() != 0) {
        ESP_LOGE(TAG, "Audio driver init failed");
        return;
    }

    ESP_LOGI(TAG, "Hardware initialized (PCM passthrough mode)");

    /* 延迟 2 秒让用户看到启动日志 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 创建测试任务 */
    xTaskCreate(vad_test_task, "vad_test", 4096, NULL, 5, NULL);

    /* 主任务空闲 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
