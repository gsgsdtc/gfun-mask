/*
 * @doc     docs/modules/audio-capture/design/01-esp32-audio-capture-design.md §4.1
 * @purpose 麦克风驱动抽象接口：
 *   - 支持后续自研硬件适配
 *   - 避免与 ESP32-S3-BOX-Lite 硬编码耦合
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief 麦克风驱动操作函数表
 *
 * 实现此接口以支持不同硬件平台。
 */
typedef struct audio_driver_ops {
    /**
     * @brief 初始化音频硬件（I2S、ADC、I2C 等）
     * @return 0 成功，负值失败
     */
    int (*init)(void);

    /**
     * @brief 启动音频采集
     * @return 0 成功，负值失败
     */
    int (*start)(void);

    /**
     * @brief 停止音频采集
     * @return 0 成功，负值失败
     */
    int (*stop)(void);

    /**
     * @brief 读取 PCM 音频数据
     * @param buf     输出缓冲区
     * @param samples 请求读取的采样点数
     * @return 实际读取的采样点数，负值表示失败
     */
    int (*read)(int16_t *buf, size_t samples);

    /**
     * @brief 释放硬件资源
     */
    void (*deinit)(void);
} audio_driver_ops_t;

/**
 * @brief 注册音频驱动
 *
 * @param ops 驱动操作函数表
 */
void audio_driver_register(const audio_driver_ops_t *ops);

/**
 * @brief 获取当前注册的驱动
 *
 * @return 驱动操作函数表指针，未注册时返回 NULL
 */
const audio_driver_ops_t* audio_driver_get(void);

/**
 * @brief 检查驱动是否已注册
 *
 * @return true 已注册，false 未注册
 */
bool audio_driver_is_registered(void);