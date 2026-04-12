/*
 * @purpose GMF VAD Element 实现
 * @brief   使用 ESP-SR AFE Manager 集成 VADNet（深度学习模型）
 *          通过 AFE Manager 正确处理 PSRAM/Flash 缓存
 *
 * 架构：
 *   Mic IO -> [VAD Element: AFE Manager + VADNet] -> PCM Encoder
 *                     |
 *                     └── VAD 事件回调 -> 自动停止
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_audio_element.h"
#include "esp_gmf_port.h"
#include "esp_gmf_afe_manager.h"
#include "esp_gmf_afe.h"
#include "esp_vad.h"           /* For VAD_MODE_3 */
#include "esp_vadn_models.h"   /* For ESP_VADN_PREFIX */
#include "esp_wn_models.h"     /* For ESP_WN_PREFIX, WAKENET_DETECTED */
#include "model_path.h"        /* For esp_srmodel_init */

#include "gmf_vad_el.h"

#define TAG "GMF_VAD_EL"

/* ── 内部状态结构体 ────────────────────────────────────────────── */

/* 环形缓冲区用于解耦 GMF Pipeline 和 AFE Manager */
typedef struct {
    int16_t *buffer;          /* 缓冲区 */
    size_t   size;            /* 缓冲区大小（样本数） */
    volatile size_t write_idx;  /* 写索引 */
    volatile size_t read_idx;   /* 读索引 */
    size_t   chunk_size;      /* AFE 期望的块大小（样本数） */
    SemaphoreHandle_t mutex;  /* 互斥锁 */
} vad_ringbuf_t;

typedef struct {
    esp_gmf_audio_element_t      parent;          /* GMF Audio Element 基类 */
    esp_gmf_afe_manager_handle_t afe_manager;     /* AFE Manager（管理 VADNet） */

    /* VAD 配置 */
    int                          mode;
    int                          min_speech_ms;
    int                          min_silence_ms;

    /* WakeNet 配置 */
    bool                         enable_wakenet;
    char                        *wakenet_model_name;

    /* 调试配置 */
    bool                         debug_log;

    /* 运行时状态 */
    bool                         speech_started;
    uint32_t                     silence_count;
    bool                         wakeword_detected;  /* 唤醒词已检测 */

    /* 回调 */
    void (*callback)(vad_event_t event, void *ctx);
    void *callback_ctx;

    /* 音频缓冲区 */
    int16_t                     *audio_buffer;
    size_t                       buffer_size;

    /* 模型列表（需要保存以便释放） */
    srmodel_list_t              *models;

    /* 环形缓冲区（解耦 GMF 和 AFE） */
    vad_ringbuf_t               ringbuf;
} gmf_vad_el_t;

/* ── 静态函数前向声明 ─────────────────────────────────────────── */

static esp_gmf_err_t _vad_destroy(esp_gmf_element_handle_t self);
static esp_gmf_job_err_t _vad_open(esp_gmf_element_handle_t self, void *para);
static esp_gmf_job_err_t _vad_close(esp_gmf_element_handle_t self, void *para);
static esp_gmf_job_err_t _vad_process(esp_gmf_element_handle_t self, void *para);

/* ── AFE 结果回调 ──────────────────────────────────────────────── */

/* 全局计数器用于确认回调是否被调用 */
static volatile int s_callback_enter_count = 0;

static void _afe_result_callback(afe_fetch_result_t *result, void *user_ctx)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)user_ctx;

    /* 增加进入计数 */
    s_callback_enter_count++;

    if (!result || !vad) {
        ESP_LOGW(TAG, "VAD callback called with NULL: result=%p, vad=%p", result, vad);
        return;
    }

    /* 调试日志：根据debug_log开关控制 */
    static int frame_count = 0;
    static int last_vad_state = -1;
    static int last_wakeup_state = -1;
    frame_count++;

    bool state_changed = (result->vad_state != last_vad_state) ||
                         (result->wakeup_state != last_wakeup_state);

    /* 只有开启debug_log且状态变化或每500帧时才打印 */
    if (vad->debug_log && (state_changed || frame_count % 500 == 0)) {
        ESP_LOGI(TAG, "VAD #%d: vad=%s, wakeup=%s",
                 frame_count,
                 result->vad_state == VAD_SPEECH ? "SPEECH" : "SILENCE",
                 result->wakeup_state == WAKENET_DETECTED ? "DETECTED" : "none");
        last_vad_state = result->vad_state;
        last_wakeup_state = result->wakeup_state;
    }

    /* 处理唤醒词事件 */
    if (vad->enable_wakenet && !vad->wakeword_detected) {
        if (result->wakeup_state == WAKENET_DETECTED) {
            vad->wakeword_detected = true;
            ESP_LOGI(TAG, "WakeWord detected!");
            if (vad->callback) {
                vad->callback(VAD_EVENT_WAKEWORD, vad->callback_ctx);
            }
        }
    }

    /* 处理 VAD 事件 */
    if (result->vad_state == VAD_SPEECH && !vad->speech_started) {
        vad->speech_started = true;
        vad->silence_count = 0;
        if (vad->callback) {
            vad->callback(VAD_EVENT_SPEECH_START, vad->callback_ctx);
        }
    }

    if (result->vad_state == VAD_SILENCE && vad->speech_started) {
        vad->silence_count++;

        /* 计算需要的静音帧数 */
        int silence_frames = vad->min_silence_ms / 30; /* VAD 通常 30ms 一帧 */

        if (vad->silence_count >= silence_frames) {
            vad->speech_started = false;
            vad->silence_count = 0;
            if (vad->callback) {
                vad->callback(VAD_EVENT_SPEECH_END, vad->callback_ctx);
            }
        }
    }
}

/* ── 环形缓冲区操作 ─────────────────────────────────────────────── */

static inline size_t ringbuf_available(vad_ringbuf_t *rb)
{
    size_t write_idx = rb->write_idx;
    size_t read_idx = rb->read_idx;
    if (write_idx >= read_idx) {
        return write_idx - read_idx;
    } else {
        return rb->size - read_idx + write_idx;
    }
}

static inline size_t ringbuf_space(vad_ringbuf_t *rb)
{
    return rb->size - ringbuf_available(rb) - 1; /* 保留一个样本区分满/空 */
}

static size_t ringbuf_write(vad_ringbuf_t *rb, const int16_t *data, size_t samples)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t space = ringbuf_space(rb);
    size_t to_write = (samples < space) ? samples : space;

    for (size_t i = 0; i < to_write; i++) {
        rb->buffer[rb->write_idx] = data[i];
        rb->write_idx = (rb->write_idx + 1) % rb->size;
    }
    xSemaphoreGive(rb->mutex);
    return to_write;
}

static size_t ringbuf_read(vad_ringbuf_t *rb, int16_t *data, size_t samples, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    size_t total_read = 0;

    while (total_read < samples) {
        xSemaphoreTake(rb->mutex, portMAX_DELAY);
        size_t available = ringbuf_available(rb);
        size_t still_needed = samples - total_read;
        size_t to_read = (available < still_needed) ? available : still_needed;

        for (size_t i = 0; i < to_read; i++) {
            data[total_read + i] = rb->buffer[rb->read_idx];
            rb->read_idx = (rb->read_idx + 1) % rb->size;
        }
        xSemaphoreGive(rb->mutex);

        total_read += to_read;

        if (total_read >= samples) {
            break;
        }

        /* 等待更多数据 */
        vTaskDelay(pdMS_TO_TICKS(10));
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            break;
        }
    }

    return total_read * sizeof(int16_t); /* 返回字节数 */
}

/* ── AFE 读取回调 ──────────────────────────────────────────────── */

static int32_t _afe_read_callback(void *buffer, int buf_sz, void *user_ctx, uint32_t ticks)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)user_ctx;

    /* 调试统计 */
    static int total_calls = 0;
    static int total_bytes = 0;
    total_calls++;

    /* 从环形缓冲区读取数据（而不是直接从 GMF Port）
     * 使用较长的超时时间，确保能获取完整数据块
     */
    size_t samples_needed = buf_sz / sizeof(int16_t);
    /* 使用更长的超时，确保能等到数据 */
    uint32_t timeout_ms = 5000; /* 5秒超时 */

    size_t bytes_read = ringbuf_read(&vad->ringbuf, buffer, samples_needed, timeout_ms);
    total_bytes += bytes_read;

    /* 如果读取的数据不足，记录警告 */
    if (bytes_read < (size_t)buf_sz) {
        static int fail_cnt = 0;
        if (++fail_cnt % 100 == 0) {
            ESP_LOGW(TAG, "AFE read incomplete: %d/%d bytes (total calls: %d)", bytes_read, buf_sz, total_calls);
        }
    } else if (vad->debug_log) {
        static int dbg_cnt = 0;
        if (++dbg_cnt % 200 == 0) {
            ESP_LOGI(TAG, "AFE read: calls=%d, total=%dKB",
                     total_calls, total_bytes / 1024);
        }
    }

    return bytes_read;
}

/* ── 静态函数 ──────────────────────────────────────────────────── */

static esp_gmf_err_t _vad_new(void *cfg, esp_gmf_obj_handle_t *handle)
{
    /* _vad_new 是 GMF 框架调用的对象创建回调
     * 它应该直接分配和初始化对象，而不是调用 gmf_vad_el_init
     * 否则会导致无限递归
     */
    ESP_GMF_NULL_CHECK(TAG, cfg, return ESP_GMF_ERR_INVALID_ARG);
    ESP_GMF_NULL_CHECK(TAG, handle, return ESP_GMF_ERR_INVALID_ARG);

    *handle = NULL;

    gmf_vad_el_t *vad = esp_gmf_oal_calloc(1, sizeof(gmf_vad_el_t));
    ESP_GMF_MEM_VERIFY(TAG, vad, return ESP_GMF_ERR_MEMORY_LACK,
                       "gmf_vad_el_t", sizeof(gmf_vad_el_t));

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)vad;
    obj->new_obj = _vad_new;
    obj->del_obj = _vad_destroy;
    esp_gmf_obj_set_tag(obj, "vad");

    /* 配置端口属性 */
    esp_gmf_element_cfg_t el_cfg = {0};
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(el_cfg.in_attr,
                                     ESP_GMF_EL_PORT_CAP_SINGLE, 16, 0,
                                     ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, 960);
    ESP_GMF_ELEMENT_OUT_PORT_ATTR_SET(el_cfg.out_attr,
                                      ESP_GMF_EL_PORT_CAP_SINGLE, 16, 0,
                                      ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, 960);

    /* 拷贝配置 */
    gmf_vad_el_cfg_t *config = (gmf_vad_el_cfg_t *)cfg;
    gmf_vad_el_cfg_t *cfg_copy = esp_gmf_oal_calloc(1, sizeof(gmf_vad_el_cfg_t));
    ESP_GMF_MEM_VERIFY(TAG, cfg_copy,
                       { esp_gmf_oal_free(vad); return ESP_GMF_ERR_MEMORY_LACK; },
                       "gmf_vad_el_cfg_t", sizeof(gmf_vad_el_cfg_t));
    memcpy(cfg_copy, config, sizeof(gmf_vad_el_cfg_t));
    esp_gmf_obj_set_config(obj, cfg_copy, sizeof(gmf_vad_el_cfg_t));

    /* 初始化 Audio Element 基类 */
    esp_gmf_err_t ret = esp_gmf_audio_el_init(vad, &el_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to init audio element: %d", ret);
        esp_gmf_oal_free(cfg_copy);
        esp_gmf_oal_free(vad);
        return ret;
    }

    /* 绑定回调 */
    ESP_GMF_ELEMENT_GET(vad)->ops.open = _vad_open;
    ESP_GMF_ELEMENT_GET(vad)->ops.close = _vad_close;
    ESP_GMF_ELEMENT_GET(vad)->ops.process = _vad_process;

    *handle = obj;
    ESP_LOGD(TAG, "VAD Element created via _vad_new: %p", vad);
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _vad_destroy(esp_gmf_element_handle_t self)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;

    if (vad->afe_manager) {
        esp_gmf_afe_manager_destroy(vad->afe_manager);
        vad->afe_manager = NULL;
    }

    if (vad->audio_buffer) {
        esp_gmf_oal_free(vad->audio_buffer);
        vad->audio_buffer = NULL;
    }

    /* 释放环形缓冲区 */
    if (vad->ringbuf.buffer) {
        esp_gmf_oal_free(vad->ringbuf.buffer);
        vad->ringbuf.buffer = NULL;
    }
    if (vad->ringbuf.mutex) {
        vSemaphoreDelete(vad->ringbuf.mutex);
        vad->ringbuf.mutex = NULL;
    }

    /* 释放 SR 模型 */
    if (vad->models) {
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
    }

    void *cfg = OBJ_GET_CFG(self);
    if (cfg) {
        esp_gmf_oal_free(cfg);
    }

    esp_gmf_audio_el_deinit(self);
    esp_gmf_oal_free(self);
    return ESP_GMF_ERR_OK;
}

static esp_gmf_job_err_t _vad_open(esp_gmf_element_handle_t self, void *para)
{
    ESP_LOGI(TAG, "_vad_open: entering");
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;
    if (!vad) {
        ESP_LOGE(TAG, "_vad_open: vad is NULL");
        return ESP_GMF_JOB_ERR_FAIL;
    }

    ESP_LOGI(TAG, "_vad_open: getting config");
    gmf_vad_el_cfg_t *cfg = (gmf_vad_el_cfg_t *)OBJ_GET_CFG(self);
    if (!cfg) {
        ESP_LOGE(TAG, "_vad_open: cfg is NULL");
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 1. 加载 SR 模型（从 "model" flash 分区） */
    ESP_LOGI(TAG, "_vad_open: loading SR models from 'model' partition");
    vad->models = esp_srmodel_init("model");
    if (!vad->models) {
        ESP_LOGE(TAG, "Failed to load SR models from 'model' partition");
        return ESP_GMF_JOB_ERR_FAIL;
    }
    ESP_LOGI(TAG, "SR models loaded: %d model(s)", vad->models->num);

    /* 筛选 VADNet 模型（必需） */
    char *vadn_model = esp_srmodel_filter(vad->models, ESP_VADN_PREFIX, NULL);
    if (!vadn_model) {
        ESP_LOGE(TAG, "VADNet model not found in 'model' partition");
        ESP_LOGE(TAG, "Please ensure:");
        ESP_LOGE(TAG, "  1. sdkconfig has CONFIG_SR_VADN_VADNET1_MEDIUM=y");
        ESP_LOGE(TAG, "  2. srmodels.bin is flashed to partition 'model' at 0x150000");
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
        return ESP_GMF_JOB_ERR_FAIL;
    }
    ESP_LOGI(TAG, "VADNet model found: %s", vadn_model);

    /* 2. 创建 AFE 配置 */
    afe_config_t *afe_cfg = afe_config_init("M", vad->models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "Failed to init AFE config");
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 配置 VAD - 必须使用 VADNet 模型 */
    afe_cfg->vad_init = true;
    afe_cfg->vad_model_name = vadn_model;
    ESP_LOGI(TAG, "Using VADNet model: %s", vadn_model);
    afe_cfg->vad_min_speech_ms = cfg->min_speech_ms;
    afe_cfg->vad_min_noise_ms = cfg->min_silence_ms;
    afe_cfg->vad_mode = cfg->mode;  /* VAD 敏感度模式 (0-3) */

    /* 配置 WakeNet（唤醒词检测） */
    vad->enable_wakenet = cfg->enable_wakenet;
    if (cfg->enable_wakenet && cfg->wakenet_model) {
        char *wn_model = esp_srmodel_filter(vad->models, ESP_WN_PREFIX, cfg->wakenet_model);
        if (wn_model) {
            afe_cfg->wakenet_init = true;
            afe_cfg->wakenet_model_name = wn_model;
            ESP_LOGI(TAG, "WakeNet enabled: %s", wn_model);
        } else {
            ESP_LOGW(TAG, "WakeNet model '%s' not found, disabling", cfg->wakenet_model);
            afe_cfg->wakenet_init = false;
            vad->enable_wakenet = false;
        }
    } else {
        afe_cfg->wakenet_init = false;
    }

    /* 禁用 AEC，但保留 SE（语音增强）
     * 注意：SE 任务负责将数据从 rb_in 移动到 rb_out，
     * 如果禁用 SE，afe_fetch 将永远阻塞在 rb_out 读取上
     */
    afe_cfg->aec_init = false;
    afe_cfg->se_init = true;  /* 必须启用，否则数据无法流向 rb_out */

    /* 3. 初始化状态和配置 */
    vad->speech_started = false;
    vad->silence_count = 0;
    vad->wakeword_detected = false;
    vad->callback = cfg->callback;
    vad->callback_ctx = cfg->callback_ctx;
    vad->debug_log = cfg->debug_log;

    /* 4. 预先分配环形缓冲区（必须在 AFE Manager 创建前完成）
     * AFE 期望的块大小是 512 样本（从 AFE 配置可知）
     */
    size_t chunk_size = 512; /* AFE 默认块大小 */
    vad->ringbuf.chunk_size = chunk_size;
    vad->ringbuf.size = chunk_size * 8; /* 8 个块的缓冲区 */
    vad->ringbuf.buffer = esp_gmf_oal_calloc(vad->ringbuf.size, sizeof(int16_t));
    vad->ringbuf.write_idx = 0;
    vad->ringbuf.read_idx = 0;
    vad->ringbuf.mutex = xSemaphoreCreateMutex();

    if (!vad->ringbuf.buffer || !vad->ringbuf.mutex) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        if (vad->ringbuf.buffer) esp_gmf_oal_free(vad->ringbuf.buffer);
        if (vad->ringbuf.mutex) vSemaphoreDelete(vad->ringbuf.mutex);
        memset(&vad->ringbuf, 0, sizeof(vad->ringbuf));
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
        return ESP_GMF_JOB_ERR_FAIL;
    }

    ESP_LOGI(TAG, "Ring buffer initialized: %d samples (chunk: %d, 8 chunks)",
             vad->ringbuf.size, chunk_size);

    /* 分配音频缓冲区（用于 GMF Port 读取） */
    vad->buffer_size = chunk_size * sizeof(int16_t);
    vad->audio_buffer = esp_gmf_oal_calloc(1, vad->buffer_size);
    if (!vad->audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        esp_gmf_oal_free(vad->ringbuf.buffer);
        vSemaphoreDelete(vad->ringbuf.mutex);
        memset(&vad->ringbuf, 0, sizeof(vad->ringbuf));
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 5. 创建 AFE Manager（它会启动 feed/fetch 任务）
     * 现在环形缓冲区已经准备好，可以安全地让 AFE 任务开始运行
     */
    ESP_LOGI(TAG, "Configuring AFE Manager callbacks:");
    ESP_LOGI(TAG, "  read_cb: %p, read_ctx: %p", _afe_read_callback, vad);
    ESP_LOGI(TAG, "  result_cb: %p, result_ctx: %p", _afe_result_callback, vad);

    esp_gmf_afe_manager_cfg_t mgr_cfg = DEFAULT_GMF_AFE_MANAGER_CFG(
        afe_cfg,
        _afe_read_callback,   /* read_cb */
        vad,                  /* read_ctx */
        _afe_result_callback, /* result_cb */
        vad                   /* result_ctx */
    );

    /* 配置 AFE Manager 任务参数 */
    mgr_cfg.fetch_task_setting.stack_size = 16 * 1024;
    mgr_cfg.fetch_task_setting.prio = 5;
    mgr_cfg.fetch_task_setting.core = 1;
    mgr_cfg.feed_task_setting.stack_size = 8 * 1024;
    mgr_cfg.feed_task_setting.prio = 5;
    mgr_cfg.feed_task_setting.core = 1;

    esp_gmf_err_t ret = esp_gmf_afe_manager_create(&mgr_cfg, &vad->afe_manager);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to create AFE manager: %d", ret);
        esp_gmf_oal_free(vad->audio_buffer);
        esp_gmf_oal_free(vad->ringbuf.buffer);
        vSemaphoreDelete(vad->ringbuf.mutex);
        memset(&vad->ringbuf, 0, sizeof(vad->ringbuf));
        afe_config_free(afe_cfg);
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* BUG FIX: esp_gmf_afe_manager_create 没有复制 result_cb 到 result_proc
     * 必须手动设置结果回调，否则 fetch_task 不会调用回调函数
     */
    ESP_LOGI(TAG, "Setting result callback after create...");
    ret = esp_gmf_afe_manager_set_result_cb(vad->afe_manager, _afe_result_callback, vad);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to set result callback: %d", ret);
        esp_gmf_afe_manager_destroy(vad->afe_manager);
        vad->afe_manager = NULL;
        esp_gmf_oal_free(vad->audio_buffer);
        esp_gmf_oal_free(vad->ringbuf.buffer);
        vSemaphoreDelete(vad->ringbuf.mutex);
        memset(&vad->ringbuf, 0, sizeof(vad->ringbuf));
        afe_config_free(afe_cfg);
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
        return ESP_GMF_JOB_ERR_FAIL;
    }
    ESP_LOGI(TAG, "Result callback set successfully");

    afe_config_free(afe_cfg);

    ESP_LOGI(TAG, "VAD Element opened: model=%s, min_speech=%dms, min_silence=%dms",
             vadn_model ? vadn_model : "WebRTC", cfg->min_speech_ms, cfg->min_silence_ms);

    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t _vad_close(esp_gmf_element_handle_t self, void *para)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;

    if (vad->afe_manager) {
        esp_gmf_afe_manager_destroy(vad->afe_manager);
        vad->afe_manager = NULL;
    }

    if (vad->audio_buffer) {
        esp_gmf_oal_free(vad->audio_buffer);
        vad->audio_buffer = NULL;
    }

    /* 释放环形缓冲区 */
    if (vad->ringbuf.buffer) {
        esp_gmf_oal_free(vad->ringbuf.buffer);
        vad->ringbuf.buffer = NULL;
    }
    if (vad->ringbuf.mutex) {
        vSemaphoreDelete(vad->ringbuf.mutex);
        vad->ringbuf.mutex = NULL;
    }

    /* 释放 SR 模型 */
    if (vad->models) {
        esp_srmodel_deinit(vad->models);
        vad->models = NULL;
    }

    ESP_LOGI(TAG, "VAD Element closed");
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t _vad_process(esp_gmf_element_handle_t self, void *para)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;
    esp_gmf_port_handle_t in_port = ESP_GMF_ELEMENT_GET(self)->in;
    esp_gmf_port_handle_t out_port = ESP_GMF_ELEMENT_GET(self)->out;

    esp_gmf_payload_t *in_load = NULL;
    esp_gmf_job_err_t out_err = ESP_GMF_JOB_ERR_OK;

    /* 从输入读取 PCM 数据 */
    esp_gmf_err_io_t port_ret = esp_gmf_port_acquire_in(in_port, &in_load,
                                                         vad->buffer_size,
                                                         ESP_GMF_MAX_DELAY);
    if (port_ret != ESP_GMF_IO_OK) {
        if (port_ret == ESP_GMF_IO_ABORT) {
            return ESP_GMF_JOB_ERR_OK;
        }
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 将数据写入环形缓冲区供 AFE Manager 消费 */
    if (in_load->valid_size > 0) {
        size_t samples = in_load->valid_size / sizeof(int16_t);
        size_t written = ringbuf_write(&vad->ringbuf, (int16_t *)in_load->buf, samples);

        /* 每500次处理打印一次调试信息（仅debug模式） */
        if (vad->debug_log) {
            static int dbg_cnt = 0;
            if (++dbg_cnt % 500 == 0) {
                ESP_LOGI(TAG, "Process: wrote %d samples, avail=%d, speech=%d",
                         written, ringbuf_available(&vad->ringbuf), vad->speech_started);
            }
        }

        /* 如果环形缓冲区满了，说明 AFE 消费跟不上，记录警告 */
        if (written < samples) {
            ESP_LOGW(TAG, "Ring buffer full, dropped %d samples", samples - written);
        }
    }

    /* 数据传递给下游（可选，如果下游需要原始音频） */
    if (out_port && in_load->valid_size > 0) {
        esp_gmf_payload_t *out_load = NULL;
        port_ret = esp_gmf_port_acquire_out(out_port, &out_load,
                                             in_load->valid_size,
                                             ESP_GMF_MAX_DELAY);
        if (port_ret == ESP_GMF_IO_OK) {
            memcpy(out_load->buf, in_load->buf, in_load->valid_size);
            out_load->valid_size = in_load->valid_size;
            esp_gmf_port_release_out(out_port, out_load, ESP_GMF_MAX_DELAY);
        }
    }

    if (in_load->is_done) {
        out_err = ESP_GMF_JOB_ERR_DONE;
    }

    esp_gmf_port_release_in(in_port, in_load, ESP_GMF_MAX_DELAY);
    return out_err;
}

/* ── 公开接口 ──────────────────────────────────────────────────── */

esp_gmf_err_t gmf_vad_el_init(gmf_vad_el_cfg_t *config, esp_gmf_element_handle_t *handle)
{
    ESP_GMF_NULL_CHECK(TAG, config, return ESP_GMF_ERR_INVALID_ARG);
    ESP_GMF_NULL_CHECK(TAG, handle, return ESP_GMF_ERR_INVALID_ARG);

    *handle = NULL;

    gmf_vad_el_t *vad = esp_gmf_oal_calloc(1, sizeof(gmf_vad_el_t));
    ESP_GMF_MEM_VERIFY(TAG, vad, return ESP_GMF_ERR_MEMORY_LACK,
                       "gmf_vad_el_t", sizeof(gmf_vad_el_t));

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)vad;
    obj->new_obj = _vad_new;
    obj->del_obj = _vad_destroy;
    esp_gmf_obj_set_tag(obj, "vad");

    /* 配置端口属性 */
    esp_gmf_element_cfg_t el_cfg = {0};
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(el_cfg.in_attr,
                                     ESP_GMF_EL_PORT_CAP_SINGLE, 16, 0,
                                     ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, 960);
    ESP_GMF_ELEMENT_OUT_PORT_ATTR_SET(el_cfg.out_attr,
                                      ESP_GMF_EL_PORT_CAP_SINGLE, 16, 0,
                                      ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, 960);

    /* 拷贝配置 */
    gmf_vad_el_cfg_t *cfg = esp_gmf_oal_calloc(1, sizeof(gmf_vad_el_cfg_t));
    ESP_GMF_MEM_VERIFY(TAG, cfg,
                       { esp_gmf_oal_free(vad); return ESP_GMF_ERR_MEMORY_LACK; },
                       "gmf_vad_el_cfg_t", sizeof(gmf_vad_el_cfg_t));
    memcpy(cfg, config, sizeof(gmf_vad_el_cfg_t));
    esp_gmf_obj_set_config(obj, cfg, sizeof(gmf_vad_el_cfg_t));

    /* 初始化 Audio Element 基类 */
    esp_gmf_err_t ret = esp_gmf_audio_el_init(vad, &el_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to init audio element: %d", ret);
        esp_gmf_oal_free(cfg);
        esp_gmf_oal_free(vad);
        return ret;
    }

    /* 绑定回调 */
    ESP_GMF_ELEMENT_GET(vad)->ops.open = _vad_open;
    ESP_GMF_ELEMENT_GET(vad)->ops.close = _vad_close;
    ESP_GMF_ELEMENT_GET(vad)->ops.process = _vad_process;

    *handle = obj;
    ESP_LOGD(TAG, "VADNet Element initialized via AFE: %p", vad);
    return ESP_GMF_ERR_OK;
}
