/*
 * @doc     docs/modules/audio-capture/design/06-gmf-core-migration-design.md §2.2
 * @purpose GMF PCM 编码 Element 实现：PCM passthrough + BLE L2CAP 发送（sink element，无输出 Port）
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_audio_element.h"
#include "esp_gmf_port.h"
#include "gmf_pcm_enc_el.h"
#include "gmf_mic_io.h"
#include "opus_encoder.h"
#include "ble_l2cap.h"

#define TAG "GMF_PCM_ENC"

#define PCM_FRAME_BYTES  GMF_PCM_FRAME_BYTES   /* 320 samples × 2 bytes/sample, defined in gmf_mic_io.h */
#define BLE_WAIT_DELAY_MS 5    /* BLE 流控轮询间隔 */

/* ── 内部状态结构体 ───────────────────────────────────────────── */

typedef struct {
    esp_gmf_audio_element_t  parent;  /* GMF 音频 Element 基类（必须第一个字段） */
} pcm_enc_el_t;

/* ── 静态函数：GMF Element 回调 ──────────────────────────────── */

static esp_gmf_err_t _enc_new(void *cfg, esp_gmf_obj_handle_t *handle)
{
    return gmf_pcm_enc_el_init(cfg, (esp_gmf_element_handle_t *)handle);
}

static esp_gmf_job_err_t _enc_open(esp_gmf_element_handle_t self, void *para)
{
    /* opus_encoder 在 audio_pipeline_init 中已初始化（此处幂等） */
    if (opus_encoder_init() != 0) {
        ESP_LOGE(TAG, "opus_encoder_init failed");
        return ESP_GMF_JOB_ERR_FAIL;
    }
    ESP_LOGI(TAG, "PCM enc element opened");
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t _enc_close(esp_gmf_element_handle_t self, void *para)
{
    /* deinit 在 audio_pipeline_deinit 中调用，此处仅返回 OK */
    ESP_LOGI(TAG, "PCM enc element closed");
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t _enc_process(esp_gmf_element_handle_t self, void *para)
{
    gmf_pcm_enc_el_cfg_t *cfg = (gmf_pcm_enc_el_cfg_t *)OBJ_GET_CFG(self);
    esp_gmf_port_handle_t in_port = ESP_GMF_ELEMENT_GET(self)->in;

    esp_gmf_job_err_t out_err = ESP_GMF_JOB_ERR_OK;
    esp_gmf_payload_t *in_load = NULL;

    /* 从输入 Port 读取 PCM 数据（触发 gmf_mic_io.acquire_read） */
    esp_gmf_err_io_t port_ret = esp_gmf_port_acquire_in(in_port, &in_load,
                                                         PCM_FRAME_BYTES,
                                                         ESP_GMF_MAX_DELAY);
    do {
        if (port_ret == ESP_GMF_IO_ABORT) {
            /* Pipeline 被外部停止 */
            out_err = ESP_GMF_JOB_ERR_OK;
            break;
        }
        if (port_ret < ESP_GMF_IO_OK) {
            ESP_LOGE(TAG, "acquire_in failed: %d", port_ret);
            out_err = ESP_GMF_JOB_ERR_FAIL;
            break;
        }

        /* PCM passthrough：调用 opus_encoder_encode（内部 memcpy） */
        uint8_t opus_buf[PCM_FRAME_BYTES + 8];  /* passthrough 输出与输入等大 */
        int opus_len = opus_encoder_encode((int16_t *)in_load->buf,
                                           opus_buf, sizeof(opus_buf));
        if (opus_len <= 0) {
            ESP_LOGW(TAG, "Encode returned %d, skipping frame", opus_len);
            break;
        }

        /* BLE 流控等待：若信道忙，轮询等待，同时检查录制状态 */
        while (!ble_l2cap_is_tx_ready()) {
            if (cfg->state == NULL || *cfg->state != AUDIO_STATE_RECORDING) {
                /* 录制已停止，不再等待 */
                out_err = ESP_GMF_JOB_ERR_DONE;
                goto _release;
            }
            vTaskDelay(pdMS_TO_TICKS(BLE_WAIT_DELAY_MS));
        }

        /* 再次检查状态（等待期间可能已停止） */
        if (cfg->state != NULL && *cfg->state != AUDIO_STATE_RECORDING) {
            out_err = ESP_GMF_JOB_ERR_DONE;
            break;
        }

        ble_l2cap_send_frame(FRAME_TYPE_AUDIO, opus_buf, (uint16_t)opus_len);

        if (in_load->is_done) {
            out_err = ESP_GMF_JOB_ERR_DONE;
        }
    } while (0);

_release:
    if (in_load != NULL) {
        esp_gmf_port_release_in(in_port, in_load, ESP_GMF_MAX_DELAY);
    }
    return out_err;
}

static esp_gmf_err_t _enc_destroy(esp_gmf_element_handle_t self)
{
    void *cfg = OBJ_GET_CFG(self);
    if (cfg) {
        esp_gmf_oal_free(cfg);
    }
    esp_gmf_audio_el_deinit(self);
    esp_gmf_oal_free(self);
    return ESP_GMF_ERR_OK;
}

/* ── 公开接口 ─────────────────────────────────────────────── */

esp_gmf_err_t gmf_pcm_enc_el_init(gmf_pcm_enc_el_cfg_t *config, esp_gmf_element_handle_t *handle)
{
    ESP_GMF_NULL_CHECK(TAG, config, return ESP_GMF_ERR_INVALID_ARG);
    ESP_GMF_NULL_CHECK(TAG, handle, return ESP_GMF_ERR_INVALID_ARG);

    *handle = NULL;
    esp_gmf_err_t ret = ESP_GMF_ERR_OK;

    pcm_enc_el_t *el = esp_gmf_oal_calloc(1, sizeof(pcm_enc_el_t));
    ESP_GMF_MEM_VERIFY(TAG, el, return ESP_GMF_ERR_MEMORY_LACK,
                       "pcm_enc_el_t", sizeof(pcm_enc_el_t));

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)el;
    obj->new_obj = _enc_new;
    obj->del_obj = _enc_destroy;
    esp_gmf_obj_set_tag(obj, "pcm_enc");

    /* 配置 Element 端口属性：只有输入 Port，无输出 Port（Sink Element） */
    esp_gmf_element_cfg_t el_cfg = {0};
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(el_cfg.in_attr,
                                     ESP_GMF_EL_PORT_CAP_SINGLE, 0, 0,
                                     ESP_GMF_PORT_TYPE_BYTE,
                                     PCM_FRAME_BYTES);
    /* out_attr 保持全零（无输出 Port 能力） */

    /* 拷贝配置 */
    gmf_pcm_enc_el_cfg_t *cfg = esp_gmf_oal_calloc(1, sizeof(gmf_pcm_enc_el_cfg_t));
    ESP_GMF_MEM_VERIFY(TAG, cfg,
                       { ret = ESP_GMF_ERR_MEMORY_LACK; goto _enc_fail; },
                       "gmf_pcm_enc_el_cfg_t", sizeof(gmf_pcm_enc_el_cfg_t));
    memcpy(cfg, config, sizeof(gmf_pcm_enc_el_cfg_t));
    esp_gmf_obj_set_config(obj, cfg, sizeof(gmf_pcm_enc_el_cfg_t));

    /* 初始化 Audio Element 基类 */
    ret = esp_gmf_audio_el_init(el, &el_cfg);
    ESP_GMF_RET_ON_NOT_OK(TAG, ret, goto _enc_fail, "Failed to init audio element");

    /* 绑定回调 */
    ESP_GMF_ELEMENT_GET(el)->ops.open    = _enc_open;
    ESP_GMF_ELEMENT_GET(el)->ops.process = _enc_process;
    ESP_GMF_ELEMENT_GET(el)->ops.close   = _enc_close;

    *handle = obj;
    ESP_LOGD(TAG, "Initialized %s-%p", OBJ_GET_TAG(obj), el);
    return ESP_GMF_ERR_OK;

_enc_fail:
    _enc_destroy(obj);
    return ret;
}
