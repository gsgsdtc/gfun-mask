/*
 * @doc     docs/modules/audio-capture/design/06-gmf-core-migration-design.md §2.2
 * @purpose 自定义 GMF IO 源：将 audio_driver_ops_t HAL 包装为 GMF IO Reader
 */

#pragma once

#include "esp_gmf_io.h"
#include "audio_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/** PCM 帧参数（16kHz 单声道，20ms/帧） */
#define GMF_PCM_FRAME_SAMPLES  320                            /**< 每帧样本数 */
#define GMF_PCM_FRAME_BYTES    (GMF_PCM_FRAME_SAMPLES * 2)   /**< 每帧字节数 (int16_t) */

/**
 * @brief  gmf_mic_io 配置
 */
typedef struct {
    const char              *name;    /**< IO 名称（用于 Pool 注册），NULL 时使用默认值 */
    const audio_driver_ops_t *driver; /**< 已注册的音频驱动 HAL */
} gmf_mic_io_cfg_t;

/**
 * @brief  初始化 gmf_mic_io 实例（用于注册到 Pool）
 *
 * @param[in]   config  配置结构体指针
 * @param[out]  io      输出 IO 句柄
 * @return  ESP_GMF_ERR_OK 成功，其他值失败
 */
esp_gmf_err_t gmf_mic_io_init(gmf_mic_io_cfg_t *config, esp_gmf_io_handle_t *io);

#ifdef __cplusplus
}
#endif
