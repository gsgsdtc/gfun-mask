/*
 * @doc     docs/modules/audio-capture/design/01-esp32-audio-capture-design.md §4.2
 * @purpose Opus 编码器封装
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── 编码参数 ─────────────────────────────────────────────── */

#define OPUS_SAMPLE_RATE    16000   /* 采样率 */
#define OPUS_CHANNELS       1       /* 声道数 */
#define OPUS_FRAME_MS       20      /* 帧大小（毫秒） */
#define OPUS_FRAME_SIZE     320     /* 每帧采样点数（20ms @ 16kHz） */
#define OPUS_BITRATE        16000   /* 目标码率（bps） */
#define OPUS_COMPLEXITY     5       /* 编码复杂度（0-10） */

/* ── 接口函数 ─────────────────────────────────────────────── */

/**
 * @brief 初始化 Opus 编码器
 * @return 0 成功，负值失败
 */
int opus_encoder_init(void);

/**
 * @brief 编码 PCM 数据
 *
 * @param pcm_in   输入 PCM 数据（20ms 帧 = 320 samples）
 * @param opus_out 输出 Opus 数据缓冲区
 * @param max_out  输出缓冲区最大大小
 * @return 编码后数据长度，失败返回负值
 */
int opus_encoder_encode(const int16_t *pcm_in, uint8_t *opus_out, size_t max_out);

/**
 * @brief 释放编码器资源
 */
void opus_encoder_deinit(void);

/**
 * @brief 获取已编码帧数
 * @return 帧计数
 */
uint32_t opus_encoder_get_frame_count(void);

/**
 * @brief 重置帧计数
 */
void opus_encoder_reset_count(void);