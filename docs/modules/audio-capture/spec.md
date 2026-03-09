# Module Spec: audio-capture

> 模块：ESP32 音频采集与编码
> 最近同步：2026-03-05
> 状态：Phase 2 开发中

---

## 1. 模块概述

实现 ESP32 端音频采集、Opus 编码、通过 BLE L2CAP 传输的功能。支持 iOS 远程控制录音启停，为后续实时语音对话奠定基础。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | BLE L2CAP 通道（ble-channel 模块） |
| 下游 | 无（音频数据传输到 iOS） |
| 输入 | I2S 麦克风音频数据、iOS 控制指令 |
| 输出 | Opus 编码音频帧 |

### 1.2 技术选型

| 组件 | 选型 | 说明 |
|------|------|------|
| MCU | ESP32-S3 | 支持 I2S、足够算力运行 Opus 编码 |
| 麦克风 | ES7210 (ESP32-S3-BOX-Lite 内置) | 4 通道 ADC，I2S 接口 |
| 音频编码 | Opus (OPUS wrapper for ESP-IDF) | 低延迟、高压缩比 |
| 开发框架 | ESP-IDF 5.x | 官方 SDK |

---

## 2. 功能规格

### 2.1 音频采集

| 参数 | 值 | 说明 |
|------|-----|------|
| 采样率 | 16 kHz | 语音场景标准 |
| 声道 | 单声道 | 无需立体声 |
| 位深 | 16 bit | PCM 格式 |
| 缓冲区 | 20 ms 帧 | 320 samples/帧 |

### 2.2 Opus 编码

| 参数 | 值 | 说明 |
|------|-----|------|
| 模式 | VoIP | 优化语音质量 |
| 码率 | 16 kbps | 平衡质量与带宽 |
| 帧大小 | 20 ms | 与采集缓冲对齐 |
| 复杂度 | 5 (0-10) | ESP32 性能平衡点 |

### 2.3 控制指令

| 指令 | 来源 | 行为 |
|------|------|------|
| START_RECORD | iOS | 启动麦克风采集和编码 |
| STOP_RECORD | iOS | 停止采集，发送 RECORD_END 确认 |

---

## 3. 模块结构

```
firmware/main/
├── audio/
│   ├── audio_driver.c/.h      # I2S 麦克风驱动抽象层
│   ├── audio_driver_es7210.c  # ES7210 具体实现
│   ├── opus_encoder.c/.h      # Opus 编码器封装
│   └── audio_pipeline.c/.h    # 采集 → 编码 → 发送流水线
└── boards/
    └── esp32_s3_box_lite.h    # 开发板引脚配置
```

---

## 4. 接口定义

### 4.1 audio_driver.h（抽象层）

```c
/**
 * @brief 麦克风驱动抽象接口
 *
 * 设计目标：支持后续自研硬件适配，避免与 ESP32-S3-BOX-Lite 硬编码耦合
 */

typedef struct audio_driver_ops {
    int (*init)(void);                      // 初始化 I2S 和 ADC
    int (*start)(void);                     // 开始采集
    int (*stop)(void);                      // 停止采集
    int (*read)(int16_t *buf, size_t len);  // 读取 PCM 数据
    void (*deinit)(void);                   // 释放资源
} audio_driver_ops_t;

/**
 * @brief 注册音频驱动
 *
 * @param ops 驱动操作函数表
 */
void audio_driver_register(const audio_driver_ops_t *ops);

/**
 * @brief 获取当前驱动
 */
const audio_driver_ops_t* audio_driver_get(void);
```

### 4.2 opus_encoder.h

```c
/**
 * @brief Opus 编码器封装
 */

#define OPUS_SAMPLE_RATE    16000
#define OPUS_CHANNELS       1
#define OPUS_FRAME_MS       20
#define OPUS_BITRATE        16000

/**
 * @brief 初始化 Opus 编码器
 */
int opus_encoder_init(void);

/**
 * @brief 编码 PCM 数据
 *
 * @param pcm_in 输入 PCM 数据（20ms 帧 = 320 samples）
 * @param opus_out 输出 Opus 数据缓冲区
 * @param max_out 输出缓冲区最大大小
 * @return 编码后数据长度，失败返回负值
 */
int opus_encoder_encode(const int16_t *pcm_in, uint8_t *opus_out, size_t max_out);

/**
 * @brief 释放编码器
 */
void opus_encoder_deinit(void);
```

### 4.3 audio_pipeline.h

```c
/**
 * @brief 音频流水线：采集 → 编码 → 发送
 */

typedef enum {
    AUDIO_STATE_IDLE,
    AUDIO_STATE_RECORDING,
    AUDIO_STATE_ERROR,
} audio_state_t;

/**
 * @brief 初始化音频流水线
 */
int audio_pipeline_init(void);

/**
 * @brief 启动录音
 */
int audio_pipeline_start(void);

/**
 * @brief 停止录音
 */
int audio_pipeline_stop(void);

/**
 * @brief 获取当前状态
 */
audio_state_t audio_pipeline_get_state(void);

/**
 * @brief 获取已发送帧数（用于 RECORD_END 确认）
 */
uint32_t audio_pipeline_get_frame_count(void);
```

---

## 5. 状态机

```
┌─────────┐  START_RECORD  ┌───────────┐
│  IDLE   │ ─────────────► │ RECORDING │
└─────────┘                └───────────┘
     ▲                          │
     │     STOP_RECORD          │
     └──────────────────────────┘
```

---

## 6. 硬件抽象层

### 6.1 ESP32-S3-BOX-Lite 配置

```c
// boards/esp32_s3_box_lite.h

#define I2S_NUM         I2S_NUM_0
#define I2S_SCK_PIN     GPIO_NUM_18
#define I2S_WS_PIN      GPIO_NUM_17
#define I2S_DATA_PIN    GPIO_NUM_16

// ES7210 I2C 配置
#define ES7210_I2C_ADDR     0x40
#define ES7210_I2C_SDA_PIN  GPIO_NUM_8
#define ES7210_I2C_SCL_PIN  GPIO_NUM_18
```

### 6.2 自研硬件适配指南

1. 创建新的驱动实现文件：`audio_driver_xxx.c`
2. 实现 `audio_driver_ops_t` 所有函数
3. 在板级配置中添加引脚定义
4. 编译时通过 Kconfig 选择驱动

---

## 7. 性能指标

| 指标 | 目标值 | 测量方法 |
|------|--------|---------|
| 采集延迟 | < 5 ms | I2S DMA 缓冲深度 |
| 编码延迟 | < 10 ms | Opus 帧大小 20ms |
| CPU 占用 | < 60% | ESP-IDF 任务监控 |
| 内存占用 | < 100 KB | 编码器 + 缓冲区 |

---

## 8. 验收状态

| 验收项 | 状态 | 备注 |
|--------|------|------|
| 麦克风采集 PCM 数据 | ⏳ | 待开发 |
| Opus 编码输出 | ⏳ | 待开发 |
| 通过 BLE 发送音频帧 | ⏳ | 待开发 |
| iOS 控制启停 | ⏳ | 待开发 |
| 驱动抽象层可移植 | ⏳ | 待验证 |

---

## 9. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-05 | feat #02 | 创建模块规格，定义硬件抽象层 |