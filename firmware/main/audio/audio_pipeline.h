/*
 * @doc     docs/modules/audio-capture/design/01-esp32-audio-capture-design.md §4.3
 * @purpose 音频流水线：采集 → 编码 → 发送
 */

#pragma once

#include <stdint.h>

/* ── 状态定义 ─────────────────────────────────────────────── */

typedef enum {
    AUDIO_STATE_IDLE,       /* 空闲状态 */
    AUDIO_STATE_RECORDING,  /* 录音中 */
    AUDIO_STATE_ERROR,      /* 错误状态 */
} audio_state_t;

/* ── 接口函数 ─────────────────────────────────────────────── */

/**
 * @brief 初始化音频流水线
 *
 * 包括驱动初始化、编码器初始化
 * @return 0 成功，负值失败
 */
int audio_pipeline_init(void);

/**
 * @brief 启动录音
 *
 * 启动音频采集、编码、发送流水线
 * @return 0 成功，负值失败
 */
int audio_pipeline_start(void);

/**
 * @brief 停止录音
 *
 * 停止流水线，发送 RECORD_END 帧
 * @return 0 成功，负值失败
 */
int audio_pipeline_stop(void);

/**
 * @brief 获取当前状态
 * @return 当前状态
 */
audio_state_t audio_pipeline_get_state(void);

/**
 * @brief 获取已发送帧数（用于 RECORD_END 确认）
 * @return 帧计数
 */
uint32_t audio_pipeline_get_frame_count(void);

/**
 * @brief 释放资源
 */
void audio_pipeline_deinit(void);