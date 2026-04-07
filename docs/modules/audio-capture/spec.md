# Module Spec: audio-capture

> 模块：ESP32 音频采集与编码
> 最近同步：2026-04-08
> 状态：Phase 7 完成（唤醒词检测 + AFE Manager）

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

### 2.4 唤醒词检测

| 参数 | 值 | 说明 |
|------|-----|------|
| 唤醒词模型 | WakeNet9 | Espressif 官方唤醒词引擎 |
| 当前模型 | hilexin | "嗨，乐鑫" 中文唤醒词 |
| 备用模型 | Jarvis | 英文唤醒词 |
| 推理引擎 | AFE Manager | `esp_gmf_afe_manager` 低层 API |
| 触发模式 | suspend/resume | 检测到唤醒词后挂起 AFE，录音完成后恢复 |
| 麦克风通道 | 单声道（M）| AFE_TYPE_SR 模式，无 AEC |
| 任务栈大小 | feed 8KB / fetch 16KB | WakeNet9 推理需要较大栈空间 |

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
│   ├── gmf_pcm_enc_el.c/.h      # GMF Element：PCM 编码 + BLE 发送
│   └── gmf_mute.c               # 静音检测辅助
├── wake_detector.c/.h           # 唤醒词检测（WakeNet9 + AFE Manager）
├── button_handler.c/.h          # 物理按键处理（录音停止触发）
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
- **唤醒词检测集成**：`wake_detector_init()` 初始化 AFE Manager，检测到唤醒词自动调用 `audio_pipeline_start()`，完成后自动恢复监听
- 对外接口（`audio_pipeline_init/start/stop`）保持不变，调用方无感知

### 4.4 wake_detector.h（唤醒词检测）

```c
typedef enum {
    WAKE_STATE_IDLE,
    WAKE_STATE_LISTENING,
    WAKE_STATE_SUSPENDED,    // 检测到唤醒词，挂起 AFE，开始录音
} wake_state_t;

esp_err_t wake_detector_init(void);       // 初始化 AFE Manager + WakeNet9
void      wake_detector_suspend(void);    // 挂起 AFE（让出 I2S）
void      wake_detector_resume(void);     // 恢复 AFE（录音完成）
void      wake_detector_deinit(void);
```

**实现要点：**
- 使用 `esp_gmf_afe_manager` 低层 API（非 `esp_gmf_afe` Element）
- 配置 `DEFAULT_GMF_AFE_MANAGER_CFG`，设置 `read_cb` 和 `result_cb`
- **关键修复**：库 `create()` 函数不读取 `cfg->result_cb`，必须手动调用 `esp_gmf_afe_manager_set_result_cb()` 注册回调
- 检测到唤醒词（`WAKENET_DETECTED`）时自动调用 `audio_pipeline_start()`
- 录音停止后自动恢复 AFE，继续监听唤醒词
- AFE feed/fetch 任务栈大小：16KB（WakeNet9 推理需求）

### 4.5 button_handler.h（按键处理）

```c
void button_handler_init(void);  // 初始化 BOOT 按钮（GPIO0）中断
```

**行为：**
- 按下 BOOT 键触发 `audio_pipeline_stop()`，停止录音
- 与唤醒词检测配合：唤醒启动录音 → 按键停止录音

---

## 5. 状态机

### 5.1 音频采集状态机

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

### 5.2 唤醒词检测状态机

```
                         ┌─────────────────────────────────┐
                         │                                 │
                         ▼                                 │
┌───────────┐    Wake    ┌───────────┐   录音完成/按键停止  ┌───────────┐
│  INITIAL  │ ─────────► │ LISTENING │ ───────────────────► │ SUSPENDED │
└───────────┘            └───────────┘                      └───────────┘
      │                        │                                  │
      │                        │ 唤醒词检测                       │ resume()
      │                        ▼                                  │
      │                 ┌───────────┐                            │
      │                 │ DETECTED  │ ──► audio_pipeline_start() │
      │                 └───────────┘                            │
      │                        │                                 │
      └────────────────────────┴─────────────────────────────────┘
                                   wake_detector_resume()
```

**状态说明：**
| 状态 | 说明 |
|------|------|
| INITIAL | AFE Manager 初始化中 |
| LISTENING | AFE 运行，监听唤醒词 |
| DETECTED | 检测到唤醒词，正在启动录音 Pipeline |
| SUSPENDED | AFE 挂起，GMF Pipeline 录音中 |

**触发条件：**
| 触发 | 行为 |
|------|------|
| 唤醒词检测 | `wake_detector_suspend()` + `audio_pipeline_start()` |
| 按键按下 | `audio_pipeline_stop()` + `wake_detector_resume()` |

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

// I2S 时钟配置（关键：ES7243E Slave 模式需要精确时钟）
#define AUDIO_I2S_FIXED_MCLK    2048000       // 16kHz × 128 = 2.048MHz
```

### 6.2 ES7243E 初始化关键约束

1. **必须先启动 I2S**（MCLK 输出），再配置 I2C 寄存器（ES7243E 需要 MCLK 才响应 I2C）
2. **I2S 时钟配置**：`fixed_mclk = 2048000`（16kHz × 128），确保与 ES7243E Slave 模式同步
3. **重新配置时钟**：I2C 初始化后调用 `i2s_set_clk()` 强制设置采样率，确保时钟稳定
4. **Soft Reset 顺序**：严格对齐 `esp-adf` 官方序列（3 次 Soft Reset + enable 流程）
5. **寄存器 0x06**：官方值 `0x03`（SCLK=MCLK/4），Slave 模式下 BCLK 由 I2S Master 提供
6. **PGA 增益**：`0x1A`（+30dB），官方推荐值

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

### Phase 7 — 唤醒词检测（完成）

| 验收项 | 状态 | 备注 |
|--------|------|------|
| WakeNet9 模型加载 | ✅ | hilexin 模型优先加载 |
| AFE Manager 初始化 | ✅ | feed_task + fetch_task |
| BUG-001 修复应用 | ✅ | `esp_gmf_afe_manager_set_result_cb()` 手动注册回调 |
| 唤醒词触发录音 | ✅ | 检测到"嗨，乐鑫"自动启动 GMF Pipeline |
| 按键停止录音 | ✅ | BOOT 按钮触发 `audio_pipeline_stop()` |
| AFE suspend/resume | ✅ | 录音时挂起 AFE，完成后恢复监听 |
| 任务栈配置 | ✅ | feed 8KB / fetch 16KB（WakeNet9 推理需求） |

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
| 2026-04-08 | feat #07 | 唤醒词检测：集成 WakeNet9 + AFE Manager，支持"嗨，乐鑫"触发录音 |
| 2026-04-08 | fix | BUG-001：应用层 workaround 修复 `result_cb` 被库忽略的问题 |
| 2026-04-08 | fix | I2S 时钟优化：`fixed_mclk=2048000` + `i2s_set_clk()` 强制同步 |
| 2026-04-08 | feat | 按键停止录音：BOOT 按钮中断处理 |
