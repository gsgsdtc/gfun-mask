/*
 * @doc     docs/modules/audio-capture/design/06-gmf-core-migration-design.md
 * @purpose 音频流水线实现（GMF-Core 版本）：采集 → 编码 → 发送
 *          内部使用 GMF Pipeline 替代手写 FreeRTOS 双任务架构，对外接口不变。
 */

#include "esp_log.h"
#include "audio_driver.h"
#include "opus_encoder.h"
#include "audio_pipeline.h"
#include "ble_l2cap.h"
#include "gmf_mic_io.h"
#include "gmf_pcm_enc_el.h"

#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"

#define TAG "AUDIO_PIPELINE"

/* ── 配置参数 ─────────────────────────────────────────────── */

#define GMF_TASK_STACK  8192
#define GMF_TASK_PRIO   5
#define GMF_TASK_CORE   1        /* 使用 Core 1，避免与 BLE 任务竞争 */

/* ── 内部状态 ─────────────────────────────────────────────── */

static volatile audio_state_t s_state = AUDIO_STATE_IDLE;

/* GMF 对象（init 阶段创建，deinit 阶段销毁） */
static esp_gmf_pool_handle_t    s_pool     = NULL;

/* GMF 对象（每次 start 创建，每次 stop 销毁） */
static esp_gmf_pipeline_handle_t s_pipeline = NULL;
static esp_gmf_task_handle_t     s_task     = NULL;

/* ── 内部辅助：创建 / 销毁 Pipeline ─────────────────────── */

static int _pipeline_create(void)
{
    /* 从 Pool 实例化一条新 Pipeline（Pool 会 dup 已注册的 IO/Element） */
    const char *el_names[] = {"pcm_enc"};
    esp_gmf_err_t ret = esp_gmf_pool_new_pipeline(s_pool, "mic_io",
                                                   el_names, 1,
                                                   NULL,    /* 无输出 IO，sink element 直接发 BLE */
                                                   &s_pipeline);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool_new_pipeline failed: %d", ret);
        s_pipeline = NULL;
        return -1;
    }

    /* 创建 GMF Task */
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = GMF_TASK_STACK;
    task_cfg.thread.prio  = GMF_TASK_PRIO;
    task_cfg.thread.core  = GMF_TASK_CORE;
    task_cfg.name = "gmf_audio";

    ret = esp_gmf_task_init(&task_cfg, &s_task);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "task_init failed: %d", ret);
        esp_gmf_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        s_task = NULL;
        return -1;
    }

    ret = esp_gmf_pipeline_bind_task(s_pipeline, s_task);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "bind_task failed: %d", ret);
        esp_gmf_task_deinit(s_task);
        esp_gmf_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        s_task = NULL;
        return -1;
    }

    ret = esp_gmf_pipeline_loading_jobs(s_pipeline);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "loading_jobs failed: %d", ret);
        esp_gmf_task_deinit(s_task);
        esp_gmf_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        s_task = NULL;
        return -1;
    }

    return 0;
}

static void _pipeline_destroy(void)
{
    if (s_task) {
        esp_gmf_task_deinit(s_task);
        s_task = NULL;
    }
    if (s_pipeline) {
        esp_gmf_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
}

/* ── 公开接口 ─────────────────────────────────────────────── */

int audio_pipeline_init(void)
{
    if (s_state != AUDIO_STATE_IDLE || s_pool != NULL) {
        ESP_LOGW(TAG, "Pipeline already initialized");
        return 0;
    }

    /* 检查驱动是否注册 */
    if (!audio_driver_is_registered()) {
        ESP_LOGE(TAG, "No audio driver registered");
        return -1;
    }

    /* 初始化音频硬件驱动（ES7243E：I2S 安装 + MCLK 输出 + I2C 配置）*/
    const audio_driver_ops_t *driver = audio_driver_get();
    if (driver->init() != 0) {
        ESP_LOGE(TAG, "Driver init failed");
        return -1;
    }

    /* 初始化 PCM passthrough 编码器 */
    if (opus_encoder_init() != 0) {
        ESP_LOGE(TAG, "Opus encoder init failed");
        driver->deinit();
        return -1;
    }

    /* 创建 GMF Pool */
    esp_gmf_err_t ret = esp_gmf_pool_init(&s_pool);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool_init failed: %d", ret);
        opus_encoder_deinit();
        driver->deinit();
        return -1;
    }

    /* 注册 Mic IO 到 Pool */
    gmf_mic_io_cfg_t mic_cfg = {
        .name   = "mic_io",
        .driver = driver,
    };
    esp_gmf_io_handle_t mic_io = NULL;
    ret = gmf_mic_io_init(&mic_cfg, &mic_io);
    if (ret != ESP_GMF_ERR_OK || mic_io == NULL) {
        ESP_LOGE(TAG, "gmf_mic_io_init failed: %d", ret);
        goto _init_fail;
    }
    ret = esp_gmf_pool_register_io(s_pool, mic_io, "mic_io");
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool_register_io failed: %d", ret);
        goto _init_fail;
    }

    /* 注册 PCM Enc Element 到 Pool（传入 s_state 指针用于流控中断） */
    gmf_pcm_enc_el_cfg_t enc_cfg = {
        .state = &s_state,
    };
    esp_gmf_element_handle_t enc_el = NULL;
    ret = gmf_pcm_enc_el_init(&enc_cfg, &enc_el);
    if (ret != ESP_GMF_ERR_OK || enc_el == NULL) {
        ESP_LOGE(TAG, "gmf_pcm_enc_el_init failed: %d", ret);
        goto _init_fail;
    }
    ret = esp_gmf_pool_register_element(s_pool, enc_el, "pcm_enc");
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool_register_element failed: %d", ret);
        goto _init_fail;
    }

    ESP_LOGI(TAG, "Audio pipeline initialized (GMF)");
    return 0;

_init_fail:
    esp_gmf_pool_deinit(s_pool);
    s_pool = NULL;
    opus_encoder_deinit();
    driver->deinit();
    return -1;
}

int audio_pipeline_start(void)
{
    ESP_LOGI(TAG, "audio_pipeline_start: state=%d", s_state);

    /* ERROR 状态允许重新启动 */
    if (s_state == AUDIO_STATE_ERROR) {
        ESP_LOGW(TAG, "Pipeline in error state, resetting to idle");
        s_state = AUDIO_STATE_IDLE;
    }

    if (s_state != AUDIO_STATE_IDLE) {
        ESP_LOGW(TAG, "Pipeline not idle, current state: %d", s_state);
        return -1;
    }

    /* 重置帧计数 */
    opus_encoder_reset_count();

    /* 创建 Pipeline 和 Task */
    if (_pipeline_create() != 0) {
        return -1;
    }

    /* 设置录制状态（必须在 pipeline_run 之前，供 _enc_process 流控检查） */
    s_state = AUDIO_STATE_RECORDING;

    /* 启动 GMF Pipeline（阻塞直到 Pipeline 进入 Running 状态） */
    esp_gmf_err_t ret = esp_gmf_pipeline_run(s_pipeline);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pipeline_run failed: %d", ret);
        s_state = AUDIO_STATE_ERROR;
        _pipeline_destroy();
        return -1;
    }

    ESP_LOGI(TAG, "Audio pipeline started (GMF)");
    return 0;
}

int audio_pipeline_stop(void)
{
    if (s_state != AUDIO_STATE_RECORDING) {
        ESP_LOGW(TAG, "Pipeline not recording, state=%d", s_state);
        return -1;
    }

    /* 先设置 IDLE 状态：_enc_process 中的 BLE 流控循环会检测到此变化并退出 */
    s_state = AUDIO_STATE_IDLE;

    /* 等待 GMF Pipeline 停止（阻塞直到 Task 结束，gmf_mic_io.close 会调用 driver->stop()） */
    if (s_pipeline) {
        esp_gmf_err_t ret = esp_gmf_pipeline_stop(s_pipeline);
        if (ret != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "pipeline_stop returned: %d", ret);
        }
    }

    /* 销毁本次运行的 Pipeline 和 Task */
    _pipeline_destroy();

    /* 发送录音结束帧 */
    uint32_t total_frames = opus_encoder_get_frame_count();
    ble_l2cap_send_frame(FRAME_TYPE_RECORD_END, (uint8_t *)&total_frames, sizeof(total_frames));

    ESP_LOGI(TAG, "Audio pipeline stopped, sent %lu frames", (unsigned long)total_frames);
    return 0;
}

audio_state_t audio_pipeline_get_state(void)
{
    return s_state;
}

uint32_t audio_pipeline_get_frame_count(void)
{
    return opus_encoder_get_frame_count();
}

void audio_pipeline_deinit(void)
{
    if (s_state == AUDIO_STATE_RECORDING) {
        audio_pipeline_stop();
    }

    /* 确保 Pipeline/Task 已销毁 */
    _pipeline_destroy();

    /* 销毁 Pool（含所有注册的 IO/Element 模板） */
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }

    opus_encoder_deinit();

    const audio_driver_ops_t *driver = audio_driver_get();
    if (driver) {
        driver->deinit();
    }

    ESP_LOGI(TAG, "Audio pipeline deinitialized (GMF)");
}
