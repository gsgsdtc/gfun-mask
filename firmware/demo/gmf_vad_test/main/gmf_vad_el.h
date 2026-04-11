/*
 * @purpose GMF VAD Element 头文件
 * @brief   封装 esp-sr VADNet 为 GMF Element
 *
 * 功能：
 *   - 接收 PCM 音频数据
 *   - 使用 VADNet 检测语音/静音
 *   - 触发 VAD_START/VAD_END 事件回调
 *   - 将数据传递给下游 Element
 */

#pragma once

#include "esp_gmf_audio_element.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── VAD 事件类型 ──────────────────────────────────────────────── */

typedef enum {
    VAD_EVENT_SILENCE,       /* 静音状态 */
    VAD_EVENT_SPEECH_START,  /* 检测到语音开始 */
    VAD_EVENT_SPEECH_END,    /* 检测到语音结束（句末） */
} vad_event_t;

/* ── VAD 配置 ──────────────────────────────────────────────────── */

typedef struct {
    int mode;                /* VAD 模式 (0-3, 3最敏感) */
    int min_speech_ms;       /* 最小语音持续时间（毫秒），低于此视为噪音 */
    int min_silence_ms;      /* 最小静音持续时间（毫秒），超过此触发 SPEECH_END */
    void (*callback)(vad_event_t event, void *ctx);  /* 事件回调 */
    void *callback_ctx;      /* 回调上下文 */
} gmf_vad_el_cfg_t;

/* ── 接口函数 ──────────────────────────────────────────────────── */

/**
 * @brief 初始化 VAD Element
 * @param config 配置参数
 * @param handle 输出句柄
 * @return ESP_GMF_ERR_OK 成功
 */
esp_gmf_err_t gmf_vad_el_init(gmf_vad_el_cfg_t *config, esp_gmf_element_handle_t *handle);

#ifdef __cplusplus
}
#endif
