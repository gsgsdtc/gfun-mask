# Design: 01 - ESP32 音频采集与编码设计

> 所属模块：audio-capture
> 关联需求：docs/feat/feat-02-esp32-audio-capture-opus-encoding.md
> 关联 BLE 设计：docs/modules/ble-channel/design/02-audio-frame-protocol-design.md
> 更新日期：2026-03-05
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

实现 ESP32 端音频采集、Opus 编码、通过 BLE L2CAP 传输的完整流水线。支持 iOS 远程控制录音启停，并在硬件抽象层设计上预留自研硬件适配能力。

### 1.2 设计约束

- **硬件平台**：ESP32-S3-BOX-Lite（当前阶段），需预留自研硬件适配
- **音频参数**：16kHz 单声道，Opus 编码 16kbps
- **传输通道**：复用 feat-01 的 BLE L2CAP CoC 信道
- **实时性要求**：采集 → 编码 → 发送流水线无阻塞

---

## 2. 硬件设计

### 2.1 ESP32-S3-BOX-Lite 麦克风配置

ESP32-S3-BOX-Lite 使用 ES7210 四通道 ADC 芯片，通过 I2S 接口与 ESP32-S3 通信。

| 参数 | 值 |
|------|-----|
| ADC 芯片 | ES7210 |
| I2S 接口 | I2S_NUM_0 |
| SCK (BCLK) | GPIO_18 |
| WS (LRCK) | GPIO_17 |
| DATA (DIN) | GPIO_16 |
| I2C SDA | GPIO_8 |
| I2C SCL | GPIO_18 |

### 2.2 ES7210 初始化序列

ES7210 需要通过 I2C 配置以下寄存器：

```
1. 软件复位
2. 配置时钟分频（匹配 16kHz 采样率）
3. 配置 ADC 通道（仅启用 CH1）
4. 配置 I2S 格式（标准 Phillips 格式，16-bit）
5. 使能 ADC
```

### 2.3 I2S 配置参数

```c
i2s_config_t i2s_config = {
    .mode = I2S_MODE_MASTER | I2S_MODE_RX,
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // 单声道
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 8,
    .dma_buf_len = 320, // 20ms @ 16kHz = 320 samples
    .use_apll = true,   // 使用 APLL 提高时钟精度
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
};
```

---

## 3. 软件架构

### 3.1 分层架构

```
┌─────────────────────────────────────┐
│         Application Layer           │
│  ┌─────────────────────────────┐   │
│  │     audio_pipeline.c        │   │
│  │  (采集 → 编码 → 发送 流水线) │   │
│  └─────────────────────────────┘   │
├─────────────────────────────────────┤
│         Component Layer             │
│  ┌──────────┐  ┌──────────────┐    │
│  │audio_    │  │opus_encoder  │    │
│  │driver    │  │              │    │
│  └──────────┘  └──────────────┘    │
├─────────────────────────────────────┤
│         HAL (Hardware Abstract)     │
│  ┌──────────────────────────────┐  │
│  │ audio_driver_ops_t (接口)    │  │
│  └──────────────────────────────┘  │
│  ┌──────────────────┐              │
│  │audio_driver_     │ (可替换)     │
│  │es7210.c          │              │
│  └──────────────────┘              │
├─────────────────────────────────────┤
│         Driver Layer                │
│  I2S Driver  │  I2C Driver          │
└─────────────────────────────────────┘
```

### 3.2 任务模型

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│ I2S Task    │     │ Encoder Task│     │ BLE TX Task │
│ (高优先级)   │────►│ (中优先级)   │────►│ (低优先级)   │
└─────────────┘     └─────────────┘     └─────────────┘
      │                   │                   │
   DMA 中断            Queue              L2CAP
   驱动读取            消息传递            发送
```

**任务说明**：

| 任务 | 优先级 | 栈大小 | 职责 |
|------|--------|--------|------|
| I2S Task | 5 (高) | 4KB | 从 I2S DMA 读取 PCM 数据 |
| Encoder Task | 4 (中) | 8KB | Opus 编码 |
| BLE TX Task | 3 (低) | 4KB | 通过 L2CAP 发送音频帧 |

---

## 4. 核心模块设计

### 4.1 硬件抽象层 (audio_driver)

```c
// audio_driver.h

typedef struct audio_driver_ops {
    int (*init)(void);
    int (*start)(void);
    int (*stop)(void);
    int (*read)(int16_t *buf, size_t samples);
    void (*deinit)(void);
} audio_driver_ops_t;

// 注册与获取
void audio_driver_register(const audio_driver_ops_t *ops);
const audio_driver_ops_t* audio_driver_get(void);
```

**ES7210 实现**：

```c
// audio_driver_es7210.c

static int es7210_init(void) {
    // 1. 初始化 I2C
    // 2. 配置 ES7210 寄存器
    // 3. 初始化 I2S
    return 0;
}

static int es7210_start(void) {
    // 启动 I2S DMA
    return i2s_start(I2S_NUM);
}

static int es7210_read(int16_t *buf, size_t samples) {
    size_t bytes_read;
    esp_err_t ret = i2s_read(I2S_NUM, buf, samples * sizeof(int16_t),
                              &bytes_read, portMAX_DELAY);
    return (ret == ESP_OK) ? bytes_read / sizeof(int16_t) : -1;
}

static int es7210_stop(void) {
    return i2s_stop(I2S_NUM);
}

static void es7210_deinit(void) {
    i2s_driver_delete(I2S_NUM);
    // 释放 I2C 资源
}

static const audio_driver_ops_t es7210_ops = {
    .init = es7210_init,
    .start = es7210_start,
    .stop = es7210_stop,
    .read = es7210_read,
    .deinit = es7210_deinit,
};

// 自动注册
void audio_driver_es7210_register(void) {
    audio_driver_register(&es7210_ops);
}
```

### 4.2 Opus 编码器封装

```c
// opus_encoder.c

#include "opus.h"

static OpusEncoder *encoder = NULL;
static uint32_t frame_count = 0;

int opus_encoder_init(void) {
    int error;
    encoder = opus_encoder_create(
        OPUS_SAMPLE_RATE,      // 16000
        OPUS_CHANNELS,         // 1
        OPUS_APPLICATION_VOIP, // VoIP 模式
        &error
    );
    if (error != OPUS_OK) {
        return -1;
    }

    // 配置参数
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));

    frame_count = 0;
    return 0;
}

int opus_encoder_encode(const int16_t *pcm_in, uint8_t *opus_out, size_t max_out) {
    int len = opus_encode(encoder, pcm_in, OPUS_FRAME_SIZE,
                          opus_out, max_out);
    if (len > 0) {
        frame_count++;
    }
    return len;
}

void opus_encoder_deinit(void) {
    if (encoder) {
        opus_encoder_destroy(encoder);
        encoder = NULL;
    }
}

uint32_t opus_encoder_get_frame_count(void) {
    return frame_count;
}

void opus_encoder_reset_count(void) {
    frame_count = 0;
}
```

### 4.3 音频流水线

```c
// audio_pipeline.c

static QueueHandle_t pcm_queue = NULL;
static TaskHandle_t encoder_task = NULL;
static TaskHandle_t ble_tx_task = NULL;
static volatile audio_state_t state = AUDIO_STATE_IDLE;

#define PCM_FRAME_SIZE  320  // 20ms @ 16kHz
#define QUEUE_DEPTH     10

// PCM 数据消息
typedef struct {
    int16_t samples[PCM_FRAME_SIZE];
} pcm_frame_t;

// I2S 读取任务
static void i2s_task(void *arg) {
    const audio_driver_ops_t *driver = audio_driver_get();
    pcm_frame_t frame;

    while (state == AUDIO_STATE_RECORDING) {
        int ret = driver->read(frame.samples, PCM_FRAME_SIZE);
        if (ret == PCM_FRAME_SIZE) {
            xQueueSend(pcm_queue, &frame, portMAX_DELAY);
        }
    }
    vTaskDelete(NULL);
}

// 编码任务
static void encoder_task_func(void *arg) {
    pcm_frame_t pcm_frame;
    uint8_t opus_buf[512];
    int opus_len;

    while (state == AUDIO_STATE_RECORDING) {
        if (xQueueReceive(pcm_queue, &pcm_frame, pdMS_TO_TICKS(100)) == pdTRUE) {
            opus_len = opus_encoder_encode(pcm_frame.samples, opus_buf, sizeof(opus_buf));
            if (opus_len > 0) {
                ble_l2cap_send_frame(FRAME_TYPE_AUDIO, opus_buf, opus_len);
            }
        }
    }
    vTaskDelete(NULL);
}

int audio_pipeline_start(void) {
    if (state != AUDIO_STATE_IDLE) {
        return -1;
    }

    const audio_driver_ops_t *driver = audio_driver_get();
    if (driver->start() != 0) {
        return -1;
    }

    opus_encoder_reset_count();
    state = AUDIO_STATE_RECORDING;

    // 创建队列和任务
    pcm_queue = xQueueCreate(QUEUE_DEPTH, sizeof(pcm_frame_t));
    xTaskCreate(i2s_task, "i2s_task", 4096, NULL, 5, NULL);
    xTaskCreate(encoder_task_func, "encoder_task", 8192, NULL, 4, NULL);

    return 0;
}

int audio_pipeline_stop(void) {
    if (state != AUDIO_STATE_RECORDING) {
        return -1;
    }

    state = AUDIO_STATE_IDLE;

    // 等待任务结束
    vTaskDelay(pdMS_TO_TICKS(100));

    // 清理队列
    if (pcm_queue) {
        vQueueDelete(pcm_queue);
        pcm_queue = NULL;
    }

    // 停止驱动
    const audio_driver_ops_t *driver = audio_driver_get();
    driver->stop();

    // 发送录音结束帧
    uint32_t total_frames = opus_encoder_get_frame_count();
    ble_l2cap_send_frame(FRAME_TYPE_RECORD_END,
                         (uint8_t*)&total_frames, sizeof(total_frames));

    return 0;
}
```

---

## 5. 内存与性能估算

### 5.1 内存占用

| 组件 | 大小 | 说明 |
|------|------|------|
| I2S DMA 缓冲区 | 8 × 320 × 2 = 5.1 KB | 8 个 DMA buffer |
| PCM 队列 | 10 × 640 = 6.4 KB | 10 帧 PCM 数据 |
| Opus 编码器 | ~30 KB | 编码器状态 |
| 任务栈 | 4+8+4 = 16 KB | 三个任务栈 |
| **总计** | ~58 KB | |

### 5.2 CPU 占用估算

| 操作 | 占用 | 说明 |
|------|------|------|
| I2S 读取 | < 5% | DMA 驱动，CPU 开销小 |
| Opus 编码 | ~40-50% | 主要 CPU 消耗点 |
| BLE 发送 | < 5% | L2CAP 发送 |
| **总计** | ~50-60% | 预留余量 |

---

## 6. 控制指令处理

### 6.1 BLE 指令接收

在 `ble_l2cap.c` 中处理 iOS 发来的控制指令：

```c
// ble_l2cap.c

static void handle_control_frame(uint8_t frame_type) {
    switch (frame_type) {
        case FRAME_TYPE_CMD_START_RECORD:
            audio_pipeline_start();
            break;
        case FRAME_TYPE_CMD_STOP_RECORD:
            audio_pipeline_stop();
            break;
        default:
            ESP_LOGW(TAG, "Unknown control frame: 0x%02X", frame_type);
    }
}

// L2CAP 接收回调
static void l2cap_recv_cb(struct ble_l2cap_event *event) {
    struct os_mbuf *om = event->receive.sdu_rx;
    uint8_t *data = om->om_data;
    uint16_t len = om->om_len;

    if (len >= 3) {
        uint8_t frame_type = data[0];
        uint16_t payload_len = data[1] | (data[2] << 8);

        if (frame_type >= 0x10) {
            // 控制指令
            handle_control_frame(frame_type);
        }
    }

    os_mbuf_free_chain(om);
}
```

---

## 7. 错误处理

### 7.1 错误场景与处理

| 场景 | 检测方式 | 处理策略 |
|------|---------|---------|
| I2S 读取失败 | 返回值 < 0 | 重试 3 次，失败则停止录音并上报错误 |
| Opus 编码失败 | 返回值 < 0 | 丢弃当前帧，继续编码下一帧 |
| BLE 发送失败 | 返回值 < 0 | 丢弃当前帧，不阻塞采集 |
| 内存不足 | 创建队列/任务失败 | 停止录音，上报错误 |

### 7.2 状态恢复

- 断连后自动停止录音，释放资源
- 重连后恢复到 IDLE 状态，等待新的 START_RECORD 指令

---

## 8. 测试方案

### 8.1 单元测试

| 测试项 | 方法 | 期望结果 |
|--------|------|---------|
| 硬件抽象层接口 | Mock driver ops | 接口调用正确 |
| Opus 编码 | 输入正弦波 PCM | 输出有效 Opus 数据 |
| 流水线启停 | 调用 start/stop | 状态切换正确 |

### 8.2 集成测试

| 测试项 | 步骤 | 期望结果 |
|--------|------|---------|
| 麦克风采集 | 说话测试，查看 PCM 波形 | 波形清晰，无明显噪声 |
| Opus 编码 | 连续编码 60 秒 | 无内存泄漏，CPU 稳定 |
| BLE 传输 | iOS 接收并播放 | 音质清晰，无断续 |
| 启停控制 | iOS 发送指令 | 状态切换及时 |

### 8.3 性能测试

| 测试项 | 指标 | 工具 |
|--------|------|------|
| CPU 占用率 | < 60% | ESP-IDF 任务监控 |
| 内存占用 | < 100 KB | ESP-IDF 堆监控 |
| 编码延迟 | < 10 ms | 时间戳测量 |

---

## 9. 自研硬件适配指南

### 9.1 适配步骤

1. **创建驱动实现文件**

   ```c
   // audio_driver_xxx.c
   static int xxx_init(void) { ... }
   static int xxx_start(void) { ... }
   // 实现所有 ops 函数
   ```

2. **修改板级配置**

   ```c
   // boards/my_board.h
   #define I2S_SCK_PIN     GPIO_NUM_XX
   #define I2S_WS_PIN      GPIO_NUM_XX
   #define I2S_DATA_PIN    GPIO_NUM_XX
   ```

3. **注册驱动**

   ```c
   // main.c
   #ifdef CONFIG_AUDIO_DRIVER_XXX
   audio_driver_xxx_register();
   #endif
   ```

4. **Kconfig 配置**

   ```
   choice AUDIO_DRIVER
       bool "Select Audio Driver"
       default AUDIO_DRIVER_ES7210
       help
           Select the audio driver for your hardware.

   config AUDIO_DRIVER_ES7210
       bool "ES7210 (ESP32-S3-BOX-Lite)"
   config AUDIO_DRIVER_XXX
       bool "Custom Driver"
   endchoice
   ```

---

## 10. 影响评估

### 10.1 对现有模块的影响

| 模块 | 变更 |
|------|------|
| ble-channel | 扩展 `ble_l2cap_send_frame` 接口，新增控制帧处理 |

### 10.2 回滚方案

- 固件独立烧录，可回滚至 Phase 1 版本（仅 hello world）
- 不影响 iOS App

---

## 11. 后续扩展

- **Phase 3**：VAD 检测、心跳保活
- **性能优化**：使用 ESP32-S3 的 AI 加速器优化 Opus 编码
- **多麦克风支持**：利用 ES7210 的多通道进行波束成形