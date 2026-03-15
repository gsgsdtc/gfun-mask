# Module Spec: audio-capture

> 模块：ESP32 音频采集与编码
> 最近同步：2026-03-15
> 状态：Phase 6 完成（GMF-Core 音频流水线迁移）

---

## 1. 模块概述

实现 ESP32 端音频采集、编码、通过 BLE L2CAP 传输的功能。支持 iOS 远程控制录音启停。当前阶段使用 PCM 直传验证麦克风采集链路；后续切换为 Opus 编码。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | BLE L2CAP 通道（ble-channel 模块）接收控制指令 |
| 下游 | BLE L2CAP 通道发送音频帧给 iOS |
| 输入 | I2S 麦克风 PCM 数据、iOS 控制指令（START/STOP_RECORD） |
| 输出 | PCM 音频帧（Phase 2）/ Opus 音频帧（Phase 3） |

### 1.2 技术选型

| 组件 | 选型 | 说明 |
|------|------|------|
| MCU | ESP32-S3 | 支持 I2S + APLL，足够算力运行 Opus |
| 麦克风 ADC | **ES7243E**（非 ES7210）| ESP32-S3-BOX-Lite 板载，I2C 地址 0x10 |
| 音频接口 | I2S Legacy API (`driver/i2s.h`) | ESP-IDF 5.x 中已 deprecated，但功能正常 |
| 当前编码 | PCM 直传（passthrough） | 验证麦克风链路；确认有声音后切 Opus |
| 目标编码 | Opus (`espressif/esp-opus`) | 低延迟语音编码 |
| 流水线框架 | **ESP-GMF-Core** | 替代手写双 FreeRTOS 任务架构，对外接口不变 |
| 开发框架 | ESP-IDF 5.5.x | 官方 SDK |

---

## 2. 功能规格

### 2.1 音频采集

| 参数 | 值 | 说明 |
|------|-----|------|
| 采样率 | 16 kHz | 语音场景标准 |
| 声道（I2S） | 双声道（LEFT+RIGHT） | ES7243E 输出双声道，软件提取高能量声道 |
| 声道（输出） | 单声道 | 取 L/R 能量较高的一路 |
| 位深 | 16 bit | PCM int16 |
| 帧大小 | 320 samples（20ms） | I2S DMA 每次读取 |
| MCLK | 4.096 MHz（APLL 生成） | 256 × 16kHz |
| BCLK | 512 kHz（I2S Master） | 16bit × 2ch × 16kHz |

### 2.2 编码（当前：PCM 直传）

| 参数 | 值 | 说明 |
|------|-----|------|
| 编码格式 | 无（raw PCM int16） | 验证阶段直传 |
| 帧大小 | 320 samples | 与采集帧对齐 |
| 每帧字节数 | 640 B（payload）+ 3 B（帧头）= 643 B | 须 < CoC MTU 1024 |
| 帧率 | 50 帧/秒 | 20ms/帧 |

### 2.3 控制指令

| 指令 | Frame Type | 行为 |
|------|------------|------|
| START_RECORD | `0x10` | 启动 I2S 采集 + 启动编码/发送任务 |
| STOP_RECORD | `0x11` | 停止采集，发送 `RECORD_END`（含总帧数） |

---

## 3. 模块结构

```
firmware/main/
├── audio/
│   ├── audio_driver.c/.h        # I2S 麦克风驱动抽象层
│   ├── audio_driver_es7210.c    # ES7243E 具体实现（文件名历史遗留）
│   ├── opus_encoder.c/.h        # 编码器封装（当前 PCM 直传模式）
│   ├── audio_pipeline.c/.h      # 采集 → 编码 → 发送流水线（GMF-Core 版）
│   ├── gmf_mic_io.c/.h          # GMF Element：I2S 麦克风输入源
│   └── gmf_pcm_enc_el.c/.h      # GMF Element：PCM 编码 + BLE 发送
└── boards/
    └── esp32_s3_box_lite.h      # ESP32-S3-BOX-Lite 硬件引脚配置
```

---

## 4. 接口定义

### 4.1 audio_driver.h（抽象层）

```c
typedef struct audio_driver_ops {
    int (*init)(void);                      // 初始化 I2S 和 ADC
    int (*start)(void);                     // 开始采集
    int (*stop)(void);                      // 停止采集
    int (*read)(int16_t *buf, size_t len);  // 读取 PCM 数据（单声道）
    void (*deinit)(void);                   // 释放资源
} audio_driver_ops_t;

void audio_driver_register(const audio_driver_ops_t *ops);
const audio_driver_ops_t *audio_driver_get(void);
```

### 4.2 opus_encoder.h（当前：PCM passthrough）

```c
#define OPUS_SAMPLE_RATE    16000
#define OPUS_CHANNELS       1
#define OPUS_FRAME_MS       20
#define OPUS_FRAME_SIZE     320  // 20ms × 16000Hz = 320 samples

int  opus_encoder_init(void);
// 当前实现：直接 memcpy PCM int16，返回 640（OPUS_FRAME_SIZE × sizeof(int16_t)）
int  opus_encoder_encode(const int16_t *pcm_in, uint8_t *opus_out, size_t max_out);
void opus_encoder_deinit(void);
uint32_t opus_encoder_get_frame_count(void);
void     opus_encoder_reset_count(void);
```

### 4.3 audio_pipeline.h

```c
typedef enum {
    AUDIO_STATE_IDLE,
    AUDIO_STATE_RECORDING,
    AUDIO_STATE_ERROR,
} audio_state_t;

int          audio_pipeline_init(void);
int          audio_pipeline_start(void);      // 创建 I2S 任务 + Encoder 任务
int          audio_pipeline_stop(void);       // 停止任务，发送 RECORD_END
audio_state_t audio_pipeline_get_state(void);
uint32_t     audio_pipeline_get_frame_count(void);
```

**流水线内部实现要点（GMF-Core 版）：**
- 内部使用 GMF Pipeline 替代原手写双 FreeRTOS 任务（I2S Task + Encoder Task）
- `GmfMicIO`：GMF Source Element，读取 I2S 双声道 PCM，取高能量声道输出
- `GmfPcmEncEl`：GMF Sink Element，PCM 编码 → 等待 `ble_l2cap_is_tx_ready()` → `ble_l2cap_send_frame()`
- Pipeline 运行在独立 GMF Task（Core 1，Priority 5，Stack 8192），避免与 BLE 任务竞争
- 对外接口（`audio_pipeline_init/start/stop`）保持不变，调用方无感知

---

## 5. 状态机

```
┌──────────┐  START_RECORD  ┌───────────┐  STOP_RECORD  ┌────────────┐
│   IDLE   │ ─────────────► │ RECORDING │ ────────────► │ IDLE       │
└──────────┘                └───────────┘               └────────────┘
                                  │
                    BLE 发送持续失败（非 stall）
                                  ▼
                             ┌─────────┐
                             │  ERROR  │
                             └─────────┘
                                  │ 自动恢复（s_state = IDLE）
                                  ▼
                              IDLE（可重新 start）
```

---

## 6. 硬件抽象层

### 6.1 ESP32-S3-BOX-Lite 配置（实际引脚，已验证）

```c
// boards/esp32_s3_box_lite.h

#define AUDIO_I2S_NUM           I2S_NUM_0
#define AUDIO_I2S_MCLK_PIN      GPIO_NUM_2    // MCLK → ES7243E 时钟源
#define AUDIO_I2S_SCK_PIN       GPIO_NUM_17   // BCLK
#define AUDIO_I2S_WS_PIN        GPIO_NUM_47   // LRCK
#define AUDIO_I2S_DATA_PIN      GPIO_NUM_16   // DIN（从 ES7243E 接收）

// ES7243E I2C 配置（实际地址 0x10，非 0x40）
#define ES7210_I2C_ADDR         0x10          // 7-bit，I2C scan 确认
#define ES7210_I2C_SDA_PIN      GPIO_NUM_8
#define ES7210_I2C_SCL_PIN      GPIO_NUM_18
#define ES7210_I2C_CLK_SPEED    100000        // 100kHz

#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_DMA_BUF_COUNT     8
#define AUDIO_DMA_BUF_LEN       320           // 单位：采样点数（非字节）
```

### 6.2 ES7243E 初始化关键约束

1. **必须先启动 I2S**（MCLK 输出），再配置 I2C 寄存器（ES7243E 需要 MCLK 才响应 I2C）
2. **Soft Reset 顺序**：严格对齐 `esp-adf` 官方序列（3 次 Soft Reset + enable 流程）
3. **寄存器 0x06**：官方值 `0x03`（SCLK=MCLK/4），Slave 模式下 BCLK 由 I2S Master 提供
4. **PGA 增益**：`0x1A`（+30dB），官方推荐值

---

## 7. 性能指标（实测）

| 指标 | 实测结果 | 说明 |
|------|---------|------|
| I2S 采集 | frames=320，能量 L/R 非零 | 麦克风工作正常 |
| PCM 帧大小 | 640 B/帧（payload） | 16kHz × 20ms × 16bit |
| 帧率 | 50 fps | 20ms/帧，稳定 |
| BLE 发送 | 643 B/帧，1024 MTU | 无丢帧（无 stall 时） |

---

## 8. 验收状态

### Phase 2 — PCM 直传验证

| 验收项 | 状态 | 备注 |
|--------|------|------|
| ES7243E I2C 初始化 | ✅ | I2C scan 确认 0x10 |
| I2S 采集双声道 PCM | ✅ | 能量 L/R 均非零（有声音） |
| 自动选择高能量声道 | ✅ | 能量诊断日志每 50 帧打印 |
| PCM 直传帧格式发送 | ✅ | 每帧 643B（3B 头 + 640B PCM） |
| iOS START/STOP 控制录音 | ✅ | cmd_callback 驱动 |
| BLE L2CAP 流控（stall） | ✅ | 不阻塞流水线 |
| RECORD_END 确认帧 | ✅ | 含 uint32 总帧数 |

### Phase 6 — GMF-Core 迁移

| 验收项 | 状态 | 备注 |
|--------|------|------|
| GmfMicIO Element 实现 | ✅ | I2S 读取 + 高能量声道选择 |
| GmfPcmEncEl Element 实现 | ✅ | PCM 编码 + BLE L2CAP 发送 |
| GMF Pipeline 替代双 FreeRTOS 任务 | ✅ | Core 1 独立 Task |
| 对外接口向后兼容 | ✅ | audio_pipeline_init/start/stop 不变 |
| idf_component.yml 添加 gmf-core 依赖 | ✅ | |

### Phase 3 — Opus 编码（待完成）

| 验收项 | 状态 | 备注 |
|--------|------|------|
| 集成 espressif/esp-opus | ⏳ | 需手动 clone + 添加组件 |
| Opus 编码输出 | ⏳ | |
| iOS 解码播放 | ⏳ | |

---

## 9. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-05 | feat #02 | 创建模块规格，定义硬件抽象层 |
| 2026-03-09 | fix | 修正芯片型号：ES7210 → ES7243E（0x10），更新所有引脚定义 |
| 2026-03-09 | fix | ES7243E 初始化序列：对齐 esp-adf 官方（3次 Soft Reset），修复能量全零问题 |
| 2026-03-09 | fix | I2S 读取改为双声道 + 自动选择高能量声道 |
| 2026-03-09 | fix | DMA buf_len 恢复 320（原代码多乘了 2，导致 640 samples 的 40ms 帧） |
| 2026-03-09 | feat | PCM 直传模式：640B/帧，含能量日志（每 100 帧打印） |
| 2026-03-10 | fix | CoC MTU 提升至 1024，确保 643B PCM 帧不超限 |
| 2026-03-15 | feat #06 | GMF-Core 迁移：双 FreeRTOS 任务 → GMF Pipeline，新增 GmfMicIO / GmfPcmEncEl Element |
| 2026-03-15 | feat #06 | 对外接口保持不变，Pipeline 运行在 Core 1（Priority 5） |
