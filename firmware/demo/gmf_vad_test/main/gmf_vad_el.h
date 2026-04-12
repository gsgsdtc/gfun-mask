/*
 * @purpose GMF VAD Element 头文件
 * @brief   使用 ESP-SR AFE Manager 集成 VADNet（深度学习模型）
 *
 * 实现方式：
 *   - 通过 AFE Manager 正确管理 VADNet 模型和内存
 *   - AFE Manager 内部处理 PSRAM/Flash 缓存同步
 *   - 支持 VAD_START/VAD_END 事件回调
 *
 * 依赖：
 *   - 需要 VADNet 模型烧录到 'model' 分区（srmodels.bin）
 *   - 自动从分区加载 vadnet8_ch1 或 vadnet1_medium 模型
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
    VAD_EVENT_WAKEWORD,      /* 检测到唤醒词 */
} vad_event_t;

/* ── VAD 配置 ──────────────────────────────────────────────────── */

typedef struct {
    int mode;                /* VAD 模式 (0-3, 3最敏感) - 当前未使用 */
    int min_speech_ms;       /* 最小语音持续时间（毫秒），低于此视为噪音 */
    int min_silence_ms;      /* 最小静音持续时间（毫秒），超过此触发 SPEECH_END */
    void (*callback)(vad_event_t event, void *ctx);  /* 事件回调 */
    void *callback_ctx;      /* 回调上下文 */

    /* WakeNet 唤醒词配置（可选） */
    bool enable_wakenet;     /* 是否启用唤醒词检测 */
    const char *wakenet_model; /* 唤醒词模型名，如 "wn9_jarvis_tts" */

    /* 调试配置 */
    bool debug_log;          /* 是否启用详细调试日志（默认关闭） */
} gmf_vad_el_cfg_t;

/* ── 接口函数 ──────────────────────────────────────────────────── */

/**
 * @brief 初始化 VAD Element
 * @param config 配置参数（包含回调函数）
 * @param handle 输出 Element 句柄
 * @return ESP_GMF_ERR_OK 成功
 *
 * 说明：
 *   - 内部使用 AFE Manager 管理 VADNet 模型
 *   - 自动处理 PSRAM/Flash 缓存同步
 *   - 模型从 'model' 分区自动加载
 */
esp_gmf_err_t gmf_vad_el_init(gmf_vad_el_cfg_t *config, esp_gmf_element_handle_t *handle);

#ifdef __cplusplus
}
#endif
