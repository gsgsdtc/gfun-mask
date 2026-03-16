/*
 * @doc     docs/modules/audio-capture/design/07-wake-word-voice-activation-backend-design.md §2.3
 * @purpose BOOT 键按键处理：录音中按下则停止录音并恢复唤醒词监听
 */

#pragma once

#include "esp_err.h"

/**
 * @brief 初始化按键处理器
 *
 * 注册 BOOT 键（GPIO_NUM_0）下降沿中断，
 * 创建按键处理任务（ISR 不直接调用 pipeline 接口）。
 *
 * @return ESP_OK 成功
 */
esp_err_t button_handler_init(void);

/**
 * @brief 释放按键处理器资源
 */
void button_handler_deinit(void);
