/*
 * @doc     docs/modules/audio-capture/design/06-gmf-core-migration-design.md §2.2
 * @purpose 自定义 GMF IO 源实现：包装 audio_driver_ops_t HAL，为 GMF Pipeline 提供麦克风 PCM 数据
 */

#include <string.h>
#include "esp_log.h"
#include "esp_gmf_oal_mem.h"
#include "gmf_mic_io.h"

#define TAG "GMF_MIC_IO"

/* PCM 帧参数由 gmf_mic_io.h 的 GMF_PCM_FRAME_* 统一定义 */

/* ── 内部状态结构体 ───────────────────────────────────────────── */

typedef struct {
    esp_gmf_io_t       base;     /* GMF IO 基类（必须第一个字段） */
    bool               is_open;  /* 驱动是否已启动 */
} mic_io_stream_t;

/* ── 静态函数：GMF IO 回调 ──────────────────────────────────── */

static esp_gmf_err_t _mic_new(void *cfg, esp_gmf_obj_handle_t *io)
{
    return gmf_mic_io_init(cfg, (esp_gmf_io_handle_t *)io);
}

static esp_gmf_err_t _mic_open(esp_gmf_io_handle_t io)
{
    mic_io_stream_t *mic_io = (mic_io_stream_t *)io;
    gmf_mic_io_cfg_t *cfg = (gmf_mic_io_cfg_t *)OBJ_GET_CFG(mic_io);
    ESP_GMF_NULL_CHECK(TAG, cfg, return ESP_GMF_ERR_FAIL);
    ESP_GMF_NULL_CHECK(TAG, cfg->driver, return ESP_GMF_ERR_FAIL);

    int ret = cfg->driver->start();
    if (ret != 0) {
        ESP_LOGE(TAG, "Audio driver start failed: %d", ret);
        return ESP_GMF_ERR_FAIL;
    }
    mic_io->is_open = true;
    ESP_LOGI(TAG, "Mic IO opened");
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _mic_close(esp_gmf_io_handle_t io)
{
    mic_io_stream_t *mic_io = (mic_io_stream_t *)io;
    gmf_mic_io_cfg_t *cfg = (gmf_mic_io_cfg_t *)OBJ_GET_CFG(mic_io);
    ESP_GMF_NULL_CHECK(TAG, cfg, return ESP_GMF_ERR_FAIL);

    if (mic_io->is_open) {
        cfg->driver->stop();
        mic_io->is_open = false;
        esp_gmf_io_set_pos(io, 0);
    }
    ESP_LOGI(TAG, "Mic IO closed");
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _mic_seek(esp_gmf_io_handle_t io, uint64_t seek_byte_pos)
{
    /* 麦克风流不支持 seek */
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _mic_reset(esp_gmf_io_handle_t io)
{
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_io_t _mic_acquire_read(esp_gmf_io_handle_t handle, void *payload,
                                          uint32_t wanted_size, int block_ticks)
{
    mic_io_stream_t *mic_io = (mic_io_stream_t *)handle;
    esp_gmf_payload_t *pload = (esp_gmf_payload_t *)payload;
    gmf_mic_io_cfg_t *cfg = (gmf_mic_io_cfg_t *)OBJ_GET_CFG(mic_io);
    ESP_GMF_NULL_CHECK(TAG, cfg, return ESP_GMF_IO_FAIL);
    ESP_GMF_NULL_CHECK(TAG, pload, return ESP_GMF_IO_FAIL);
    ESP_GMF_NULL_CHECK(TAG, pload->buf, return ESP_GMF_IO_FAIL);

    size_t samples = wanted_size / sizeof(int16_t);  /* = GMF_PCM_FRAME_SAMPLES */
    int read_ret = cfg->driver->read((int16_t *)pload->buf, samples);
    if (read_ret < 0) {
        ESP_LOGE(TAG, "Audio driver read error: %d", read_ret);
        return ESP_GMF_IO_FAIL;
    }
    pload->valid_size = (size_t)read_ret * sizeof(int16_t);
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _mic_release_read(esp_gmf_io_handle_t handle, void *payload, int block_ticks)
{
    esp_gmf_payload_t *pload = (esp_gmf_payload_t *)payload;
    esp_gmf_io_update_pos(handle, pload->valid_size);
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_t _mic_delete(esp_gmf_io_handle_t io)
{
    mic_io_stream_t *mic_io = (mic_io_stream_t *)io;
    void *cfg = OBJ_GET_CFG(io);
    if (cfg) {
        esp_gmf_oal_free(cfg);
    }
    esp_gmf_io_deinit(io);
    esp_gmf_oal_free(mic_io);
    return ESP_GMF_ERR_OK;
}

/* ── 公开接口 ─────────────────────────────────────────────── */

esp_gmf_err_t gmf_mic_io_init(gmf_mic_io_cfg_t *config, esp_gmf_io_handle_t *io)
{
    ESP_GMF_NULL_CHECK(TAG, config, return ESP_GMF_ERR_INVALID_ARG);
    ESP_GMF_NULL_CHECK(TAG, io, return ESP_GMF_ERR_INVALID_ARG);
    ESP_GMF_NULL_CHECK(TAG, config->driver, return ESP_GMF_ERR_INVALID_ARG);

    *io = NULL;
    esp_gmf_err_t ret = ESP_GMF_ERR_OK;

    mic_io_stream_t *mic_io = esp_gmf_oal_calloc(1, sizeof(mic_io_stream_t));
    ESP_GMF_MEM_VERIFY(TAG, mic_io, return ESP_GMF_ERR_MEMORY_LACK,
                       "mic_io_stream_t", sizeof(mic_io_stream_t));

    mic_io->base.dir  = ESP_GMF_IO_DIR_READER;
    mic_io->base.type = ESP_GMF_IO_TYPE_BYTE;

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)mic_io;
    obj->new_obj = _mic_new;
    obj->del_obj = _mic_delete;

    /* 拷贝配置 */
    gmf_mic_io_cfg_t *cfg = esp_gmf_oal_calloc(1, sizeof(gmf_mic_io_cfg_t));
    ESP_GMF_MEM_VERIFY(TAG, cfg, { ret = ESP_GMF_ERR_MEMORY_LACK; goto _mic_fail; },
                       "gmf_mic_io_cfg_t", sizeof(gmf_mic_io_cfg_t));
    memcpy(cfg, config, sizeof(gmf_mic_io_cfg_t));
    esp_gmf_obj_set_config(obj, cfg, sizeof(gmf_mic_io_cfg_t));

    ret = esp_gmf_obj_set_tag(obj, (config->name ? config->name : "mic_io"));
    ESP_GMF_RET_ON_NOT_OK(TAG, ret, goto _mic_fail, "Failed to set mic IO tag");

    /* 绑定回调 */
    mic_io->base.open          = _mic_open;
    mic_io->base.close         = _mic_close;
    mic_io->base.seek          = _mic_seek;
    mic_io->base.reset         = _mic_reset;
    mic_io->base.acquire_read  = _mic_acquire_read;
    mic_io->base.release_read  = _mic_release_read;

    esp_gmf_io_init(obj, NULL);

    *io = obj;
    ESP_LOGD(TAG, "Initialized %s-%p", OBJ_GET_TAG(obj), mic_io);
    return ESP_GMF_ERR_OK;

_mic_fail:
    esp_gmf_obj_delete(obj);
    return ret;
}
