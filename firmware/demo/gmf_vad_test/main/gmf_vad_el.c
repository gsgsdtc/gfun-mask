/*
 * @purpose GMF VAD Element 实现
 * @brief   使用 esp-sr VADNet 检测语音/静音
 */

#include <string.h>
#include "esp_log.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_audio_element.h"
#include "esp_gmf_port.h"

#include "esp_vadn_iface.h"
#include "esp_vadn_models.h"
#include "model_path.h"

#include "gmf_vad_el.h"

#define TAG "GMF_VAD_EL"

/* ── 内部状态结构体 ────────────────────────────────────────────── */

typedef struct {
    esp_gmf_audio_element_t  parent;      /* GMF Audio Element 基类 */
    const esp_vadn_iface_t  *vad_iface;   /* VADNet 接口 */
    model_iface_data_t      *vad_model;   /* VADNet 模型实例 */
    int                      chunk_size;  /* VADNet 输入块大小 (采样点数) */

    /* VAD 状态 */
    int                      mode;
    int                      min_speech_ms;
    int                      min_silence_ms;

    /* 运行时状态 */
    int                      prev_state;      /* 上一帧状态 (VAD_SILENCE=0, VAD_SPEECH=1) */
    uint32_t                 silence_count;   /* 连续静音帧数 */
    uint32_t                 speech_count;    /* 连续语音帧数 */
    bool                     speech_started;  /* 是否已开始说话 */

    /* 回调 */
    void (*callback)(vad_event_t event, void *ctx);
    void *callback_ctx;
} gmf_vad_el_t;

/* ── 静态函数 ──────────────────────────────────────────────────── */

static esp_gmf_err_t _vad_new(void *cfg, esp_gmf_obj_handle_t *handle)
{
    return gmf_vad_el_init(cfg, (esp_gmf_element_handle_t *)handle);
}

static esp_gmf_err_t _vad_destroy(esp_gmf_element_handle_t self)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;

    if (vad->vad_model && vad->vad_iface) {
        vad->vad_iface->destroy(vad->vad_model);
        vad->vad_model = NULL;
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
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;
    gmf_vad_el_cfg_t *cfg = (gmf_vad_el_cfg_t *)OBJ_GET_CFG(self);

    /* 1. 加载 VADNet 模型 */
    char *vadn_model_name = esp_srmodel_filter(esp_srmodel_init("model"), ESP_VADN_PREFIX, NULL);
    if (!vadn_model_name) {
        ESP_LOGE(TAG, "VADNet model not found in partition");
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 2. 获取 VADNet 接口 */
    vad->vad_iface = esp_vadn_handle_from_name(vadn_model_name);
    if (!vad->vad_iface) {
        ESP_LOGE(TAG, "Failed to get VADNet interface");
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 3. 创建模型实例 */
    vad->vad_model = vad->vad_iface->create(
        vadn_model_name,
        (vad_mode_t)cfg->mode,
        1,                      /* 单声道 */
        cfg->min_speech_ms,
        cfg->min_silence_ms
    );

    if (!vad->vad_model) {
        ESP_LOGE(TAG, "Failed to create VADNet model");
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 4. 获取输入块大小 */
    vad->chunk_size = vad->vad_iface->get_samp_chunksize(vad->vad_model);

    /* 5. 初始化状态 */
    vad->prev_state = VAD_SILENCE;
    vad->silence_count = 0;
    vad->speech_count = 0;
    vad->speech_started = false;
    vad->callback = cfg->callback;
    vad->callback_ctx = cfg->callback_ctx;

    ESP_LOGI(TAG, "VAD Element opened: mode=%d, chunk_size=%d samples, min_speech=%dms, min_silence=%dms",
             cfg->mode, vad->chunk_size, cfg->min_speech_ms, cfg->min_silence_ms);

    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t _vad_close(esp_gmf_element_handle_t self, void *para)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;

    if (vad->vad_model && vad->vad_iface) {
        vad->vad_iface->destroy(vad->vad_model);
        vad->vad_model = NULL;
    }

    ESP_LOGI(TAG, "VAD Element closed");
    return ESP_GMF_JOB_ERR_OK;
}

/* 计算需要多少帧的静音才算达到 min_silence_ms
 * 假设每帧 30ms (VADNet 通常使用 30ms 帧长) */
#define VAD_FRAME_MS 30

static esp_gmf_job_err_t _vad_process(esp_gmf_element_handle_t self, void *para)
{
    gmf_vad_el_t *vad = (gmf_vad_el_t *)self;
    gmf_vad_el_cfg_t *cfg = (gmf_vad_el_cfg_t *)OBJ_GET_CFG(self);
    esp_gmf_port_handle_t in_port = ESP_GMF_ELEMENT_GET(self)->in;
    esp_gmf_port_handle_t out_port = ESP_GMF_ELEMENT_GET(self)->out;

    esp_gmf_payload_t *in_load = NULL;
    esp_gmf_job_err_t out_err = ESP_GMF_JOB_ERR_OK;

    /* 计算需要的字节数 */
    int chunk_bytes = vad->chunk_size * sizeof(int16_t);

    /* 从输入读取 PCM 数据 */
    esp_gmf_err_io_t port_ret = esp_gmf_port_acquire_in(in_port, &in_load,
                                                         chunk_bytes,
                                                         ESP_GMF_MAX_DELAY);
    if (port_ret != ESP_GMF_IO_OK) {
        if (port_ret == ESP_GMF_IO_ABORT) {
            return ESP_GMF_JOB_ERR_OK;
        }
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /* 确保数据足够 */
    if (in_load->valid_size < chunk_bytes) {
        esp_gmf_port_release_in(in_port, in_load, ESP_GMF_MAX_DELAY);
        return ESP_GMF_JOB_ERR_OK;
    }

    /* VAD 检测 */
    vad_state_t curr_state = vad->vad_iface->detect(vad->vad_model, (int16_t *)in_load->buf);

    /* 状态机处理 */
    int silence_frames_needed = cfg->min_silence_ms / VAD_FRAME_MS;

    switch (curr_state) {
        case VAD_SPEECH:
            vad->silence_count = 0;
            vad->speech_count++;

            /* 语音开始检测 */
            if (!vad->speech_started && vad->speech_count >= cfg->min_speech_ms / VAD_FRAME_MS) {
                vad->speech_started = true;
                if (vad->callback) {
                    vad->callback(VAD_EVENT_SPEECH_START, vad->callback_ctx);
                }
            }
            break;

        case VAD_SILENCE:
            vad->speech_count = 0;

            if (vad->speech_started) {
                vad->silence_count++;

                /* 静音足够长，触发句末 */
                if (vad->silence_count >= silence_frames_needed) {
                    vad->speech_started = false;
                    vad->silence_count = 0;

                    if (vad->callback) {
                        vad->callback(VAD_EVENT_SPEECH_END, vad->callback_ctx);
                    }
                } else if (vad->silence_count <= 3) {
                    /* 前几帧静音打印日志 */
                    ESP_LOGD(TAG, "Silence detected (count=%d/%d)",
                             vad->silence_count, silence_frames_needed);
                }
            }
            break;

        default:
            break;
    }

    vad->prev_state = curr_state;

    /* 数据传递给下游 */
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

    /* 配置端口属性：有输入和输出 */
    esp_gmf_element_cfg_t el_cfg = {0};
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(el_cfg.in_attr,
                                     ESP_GMF_EL_PORT_CAP_SINGLE, 0, 0,
                                     ESP_GMF_PORT_TYPE_BYTE, 0);
    ESP_GMF_ELEMENT_OUT_PORT_ATTR_SET(el_cfg.out_attr,
                                      ESP_GMF_EL_PORT_CAP_SINGLE, 0, 0,
                                      ESP_GMF_PORT_TYPE_BYTE, 0);

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
    ESP_LOGD(TAG, "VAD Element initialized: %p", vad);
    return ESP_GMF_ERR_OK;
}
