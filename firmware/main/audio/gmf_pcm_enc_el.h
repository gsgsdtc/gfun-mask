/*
 * @doc     docs/modules/audio-capture/design/06-gmf-core-migration-design.md §2.2
 * @purpose 自定义 GMF PCM 编码 Element：从输入 Port 读取 PCM，passthrough 后通过 BLE L2CAP 发送
 */

#pragma once

#include "esp_gmf_element.h"
#include "audio_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  gmf_pcm_enc_el 配置
 *
 * state 指针由 audio_pipeline.c 传入，用于 BLE 流控等待时检测录制是否停止。
 */
typedef struct {
    volatile audio_state_t *state; /**< 指向 audio_pipeline.c 的 s_state，用于流控中断 */
} gmf_pcm_enc_el_cfg_t;

/**
 * @brief  初始化 gmf_pcm_enc_el 实例（用于注册到 Pool）
 *
 * @param[in]   config  配置结构体指针
 * @param[out]  handle  输出 Element 句柄
 * @return  ESP_GMF_ERR_OK 成功，其他值失败
 */
esp_gmf_err_t gmf_pcm_enc_el_init(gmf_pcm_enc_el_cfg_t *config, esp_gmf_element_handle_t *handle);

#ifdef __cplusplus
}
#endif
