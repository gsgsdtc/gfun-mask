/*
 * @doc     docs/modules/audio-capture/design/07-wake-word-voice-activation-backend-design.md
 * @purpose 唤醒词检测模块：封装 esp_gmf_afe_manager（WakeNet-only 模式）
 *          检测到 "Jarvis" 后调用 audio_pipeline_start()
 */

#pragma once

#include "esp_err.h"

/**
 * @brief 初始化唤醒词检测器
 *
 * 启动 I2S 采集，创建 AFE Manager（WakeNet-only），
 * 注册唤醒回调，开始持续监听。
 *
 * @return ESP_OK 成功，其他失败（系统降级为纯 BLE 远程控制）
 */
esp_err_t wake_detector_init(void);

/**
 * @brief 挂起唤醒词检测
 *
 * 停止 AFE Manager feed/fetch 任务并停止 I2S，
 * 让出 I2S 给 GMF 编码 Pipeline 独占使用。
 * 应在 audio_pipeline_start() 之前调用。
 */
void wake_detector_suspend(void);

/**
 * @brief 恢复唤醒词检测
 *
 * 重启 I2S 并恢复 AFE Manager feed/fetch 任务。
 * 应在 audio_pipeline_stop() 之后调用。
 */
void wake_detector_resume(void);

/**
 * @brief 释放唤醒词检测器资源
 */
void wake_detector_deinit(void);
