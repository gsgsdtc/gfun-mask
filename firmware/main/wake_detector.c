/*
 * @doc     docs/modules/audio-capture/design/07-wake-word-voice-activation-backend-design.md
 * @purpose 唤醒词检测实现：AFE Manager (WakeNet-only) + suspend/resume 模式
 *
 * 架构说明：
 *   IDLE：AFE Manager 持有 I2S，通过 read_cb 持续喂入 WakeNet9
 *   RECORDING：AFE Manager 挂起（让出 I2S），GMF Pipeline 独占采集
 *
 * 检测到 "Jarvis" 后流程：
 *   1. wake_detector_suspend()  — 挂起 AFE，停止 I2S
 *   2. audio_pipeline_start()   — GMF Pipeline 接管 I2S，开始录音
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_gmf_afe_manager.h"
#include "esp_afe_sr_iface.h"
#include "model_path.h"
#include "audio_driver.h"
#include "audio_pipeline.h"
#include "wake_detector.h"

#define TAG "WAKE_DETECTOR"

/* ── 私有状态 ─────────────────────────────────────────────── */

static esp_gmf_afe_manager_handle_t s_afe_manager = NULL;
static srmodel_list_t               *s_models      = NULL;
static afe_config_t                 *s_afe_cfg     = NULL;
static volatile bool                 s_initialized = false;

/* ── AFE Manager 回调：从 mic driver 读取 PCM 数据 ─────────── */

/*
 * @doc     §4.2 AFE Manager 配置
 * @purpose 为 AFE feed_task 提供音频数据，直接从 mic driver 读取 I2S 数据
 * @context AFE Manager 用此回调喂入 WakeNet9；I2S 必须已启动（start 后才可读）
 */
static int32_t _afe_read_cb(void *buffer, int buf_sz, void *user_ctx, uint32_t ticks)
{
    static int _rcb_cnt = 0;
    const audio_driver_ops_t *drv = audio_driver_get();
    if (!drv) {
        return 0;
    }
    /* buf_sz 单位：字节；driver->read 单位：采样点（int16_t） */
    size_t samples = (size_t)(buf_sz / sizeof(int16_t));
    int got = drv->read((int16_t *)buffer, samples);
    int32_t ret = (got > 0) ? (int32_t)(got * sizeof(int16_t)) : 0;
    _rcb_cnt++;
    if (_rcb_cnt <= 5) {
        ESP_LOGI(TAG, "AFE read_cb #%d: buf_sz=%d, samples=%u, got=%d, ret=%ld",
                 _rcb_cnt, buf_sz, (unsigned)samples, got, (long)ret);
    }
    /* 每 300 次（约 10s）打印一次 WakeNet 配置状态，方便任何时候连接都能看到 */
    if (_rcb_cnt % 300 == 150) {
        ESP_LOGI(TAG, "[DIAG] models=%d, wakenet_init=%d, model=%s, "
                      "sr=%d, total_ch=%d, mic=%d, ref=%d",
                 s_models ? s_models->num : -1,
                 s_afe_cfg ? s_afe_cfg->wakenet_init : -1,
                 (s_afe_cfg && s_afe_cfg->wakenet_model_name) ? s_afe_cfg->wakenet_model_name : "NULL",
                 s_afe_cfg ? s_afe_cfg->pcm_config.sample_rate : -1,
                 s_afe_cfg ? s_afe_cfg->pcm_config.total_ch_num : -1,
                 s_afe_cfg ? s_afe_cfg->pcm_config.mic_num : -1,
                 s_afe_cfg ? s_afe_cfg->pcm_config.ref_num : -1);
    }
    return ret;
}

/* ── AFE Manager 回调：处理检测结果 ──────────────────────────── */

/*
 * @doc     §4.1 唤醒 → 录音流程
 * @purpose 在 WAKENET_DETECTED 时触发录音启动
 * @context 此回调运行在 AFE fetch_task 中；
 *          若重复触发（已在 RECORDING）则忽略，防止状态混乱
 */
static void _afe_result_cb(afe_fetch_result_t *result, void *user_ctx)
{
    /* 调试：前 5 次和每 200 次记录一次（含 NULL result），确认 fetch 回调在运行 */
    static int _dbg_cnt = 0;
    _dbg_cnt++;
    if (_dbg_cnt <= 5 || _dbg_cnt % 200 == 0) {
        if (result) {
            ESP_LOGI(TAG, "AFE result_cb #%d: wakeup=%d, vol=%.1f dB",
                     _dbg_cnt, result->wakeup_state, result->data_volume);
        } else {
            ESP_LOGW(TAG, "AFE result_cb #%d: result=NULL", _dbg_cnt);
        }
    }

    if (!result) {
        return;
    }

    if (result->wakeup_state == WAKENET_DETECTED) {
        ESP_LOGI(TAG, "Wake word 'Jarvis' detected!");

        /* 防抖：已在录音状态则忽略 */
        if (audio_pipeline_get_state() != AUDIO_STATE_IDLE) {
            ESP_LOGW(TAG, "Already recording, ignore wake event");
            return;
        }

        /* 挂起 AFE Manager，让出 I2S 给 GMF Pipeline */
        wake_detector_suspend();

        /* 启动 GMF 编码 Pipeline */
        if (audio_pipeline_start() != 0) {
            ESP_LOGE(TAG, "audio_pipeline_start failed, resuming AFE");
            wake_detector_resume();
        }
    }
}

/* ── 公开接口 ─────────────────────────────────────────────── */

esp_err_t wake_detector_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    const audio_driver_ops_t *drv = audio_driver_get();
    if (!drv) {
        ESP_LOGE(TAG, "No audio driver registered");
        return ESP_ERR_INVALID_STATE;
    }

    /* ── 1. 加载 SR 模型（从 "model" flash 分区）── */
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — check 'model' partition exists");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SR models loaded: %d model(s)", s_models->num);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "  model[%d]: name=%s info=%s", i,
                 s_models->model_name ? s_models->model_name[i] : "NULL",
                 s_models->model_info ? s_models->model_info[i] : "NULL");
    }

    /* ── 2. 创建 AFE 配置（单麦克风，WakeNet-only）── */
    /* "M" = 单 Mic 通道，无参考信号，无 AEC */
    s_afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!s_afe_cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* 关闭 VAD（本期按键手动结束，不需要自动 VAD 结束） */
    s_afe_cfg->vad_init = false;
    /* 诊断：确认 WakeNet 是否被启用及使用的模型名 */
    ESP_LOGI(TAG, "AFE config: wakenet_init=%d, wakenet_model=%s",
             s_afe_cfg->wakenet_init,
             s_afe_cfg->wakenet_model_name ? s_afe_cfg->wakenet_model_name : "NULL");

    /* ── 3. 启动 I2S（AFE read_cb 需要 I2S 运行）── */
    if (drv->start() != 0) {
        ESP_LOGE(TAG, "mic driver start failed");
        afe_config_free(s_afe_cfg);
        esp_srmodel_deinit(s_models);
        s_afe_cfg = NULL;
        s_models  = NULL;
        return ESP_FAIL;
    }

    /* ── 4. 创建 AFE Manager（自动启动 feed/fetch 任务）── */
    esp_gmf_afe_manager_cfg_t mgr_cfg = DEFAULT_GMF_AFE_MANAGER_CFG(
        s_afe_cfg,
        _afe_read_cb,   /* read_cb  */
        NULL,           /* read_ctx */
        _afe_result_cb, /* result_cb */
        NULL            /* result_ctx */
    );
    /* WakeNet9 推理需要较大栈空间，默认 3KB 不够会导致 fetch 任务静默崩溃 */
    mgr_cfg.fetch_task_setting.stack_size = 16 * 1024;
    mgr_cfg.feed_task_setting.stack_size  = 8 * 1024;

    esp_gmf_err_t ret = esp_gmf_afe_manager_create(&mgr_cfg, &s_afe_manager);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "esp_gmf_afe_manager_create failed: %d", ret);
        drv->stop();
        afe_config_free(s_afe_cfg);
        esp_srmodel_deinit(s_models);
        s_afe_manager = NULL;
        s_afe_cfg     = NULL;
        s_models      = NULL;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Wake detector initialized, listening for 'Jarvis'...");
    return ESP_OK;
}

void wake_detector_suspend(void)
{
    if (!s_initialized || !s_afe_manager) {
        return;
    }

    /* 挂起 AFE feed/fetch 任务（停止调用 read_cb） */
    esp_gmf_afe_manager_suspend(s_afe_manager, true);

    /* 停止 I2S，让 GMF Pipeline 的 mic_io 可以干净地重启 */
    const audio_driver_ops_t *drv = audio_driver_get();
    if (drv) {
        drv->stop();
    }

    ESP_LOGI(TAG, "Wake detector suspended");
}

void wake_detector_resume(void)
{
    if (!s_initialized || !s_afe_manager) {
        return;
    }

    /* 重启 I2S（GMF Pipeline 已停止，可以安全接管） */
    const audio_driver_ops_t *drv = audio_driver_get();
    if (drv) {
        drv->start();
    }

    /* 恢复 AFE feed/fetch 任务 */
    esp_gmf_afe_manager_suspend(s_afe_manager, false);

    ESP_LOGI(TAG, "Wake detector resumed, listening for 'Jarvis'...");
}

void wake_detector_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    if (s_afe_manager) {
        esp_gmf_afe_manager_destroy(s_afe_manager);
        s_afe_manager = NULL;
    }

    const audio_driver_ops_t *drv = audio_driver_get();
    if (drv) {
        drv->stop();
    }

    if (s_afe_cfg) {
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
    }

    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Wake detector deinitialized");
}
