# Design: 07 - 唤醒词触发语音录制

> 所属模块：audio-capture
> 关联需求：docs/feat/feat-07-wake-word-voice-activation.md
> 关联前端设计：无
> 更新日期：2026-03-15
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

在 ESP32-S3 固件中集成 ESP-GMF `esp_gmf_afe_manager`（WakeNet-only 模式），实现"说 Jarvis → 自动录音 → 按键结束"的免提交互流程，并与现有 GMF 编码 Pipeline 无缝衔接。

### 1.2 设计约束

- 唤醒词固定为 "Jarvis"，使用 ESP-SR 内置模型 `wn9_jarvis_tts`，不涉及自定义训练
- 通过 `esp_gmf_afe_manager`（`espressif/gmf_ai_audio`）集成，保持 GMF 框架一致性
- AFE Manager 与 GMF 编码 Pipeline 共享同一 I2S 麦克风，通过 suspend/resume 交替使用，避免冲突
- 唤醒响应时延目标 ≤ 500ms（从唤醒词结束到录音开始）
- 按键响应时延目标 ≤ 200ms（从按键到停止录音）
- 提示音通过 ESP32-S3-BOX-Lite 板载扬声器播放（方案待定，初期可使用简单 PCM 或跳过）
- 不修改现有 `audio_pipeline.h` 对外接口，保持向后兼容

---

## 2. 接口设计

### 2.1 新增内部接口

| 函数/方法 | 说明 | 变更类型 |
|-----------|------|---------|
| `wake_detector_init()` → `esp_err_t` | 初始化 AFE Manager（WakeNet-only 模式），注册唤醒回调，启动 feed/fetch 任务 | 新增 |
| `wake_detector_suspend()` → `void` | 挂起 AFE Manager 的 feed/fetch 任务（录音期间调用，让出 I2S） | 新增 |
| `wake_detector_resume()` → `void` | 恢复 AFE Manager 的 feed/fetch 任务（录音结束后调用） | 新增 |
| `wake_detector_deinit()` → `void` | 释放 AFE Manager 资源 | 新增 |

### 2.2 复用现有接口（不修改）

| 函数/方法 | 说明 |
|-----------|------|
| `audio_pipeline_start()` → `int` | 唤醒后启动 GMF 编码 Pipeline（已有） |
| `audio_pipeline_stop()` → `int` | 按键后停止 GMF 编码 Pipeline，发送 `FRAME_TYPE_RECORD_END (0x12)`（已有） |
| `audio_pipeline_get_state()` → `audio_state_t` | 查询当前状态（已有） |

### 2.3 新增事件

| 事件 | 来源 | 处理 |
|------|------|------|
| `ESP_GMF_AFE_EVT_WAKEUP_START` | AFE Manager 回调 | 挂起 AFE → 播放提示音 → `audio_pipeline_start()` |
| 按键 GPIO 中断（BOOT 键） | GPIO ISR | `audio_pipeline_stop()` → 恢复 AFE Manager |

---

## 3. 模型设计

### 3.1 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                      ESP32-S3 固件                           │
│                                                             │
│  ┌──────────────────────────────┐                           │
│  │      wake_detector           │  [IDLE 时运行]            │
│  │   esp_gmf_afe_manager        │                           │
│  │   ┌──────────┐ ┌──────────┐  │                           │
│  │   │feed_task │ │fetch_task│  │                           │
│  │   └────┬─────┘ └────┬─────┘  │                           │
│  │        │ read_cb    │ result  │                           │
│  └────────┼────────────┼────────┘                           │
│           │            │ WAKEUP_START                       │
│           ▼            ▼                                     │
│      mic driver   ─────────► suspend AFE                    │
│      (I2S, 常开)             播放提示音                       │
│           │                  audio_pipeline_start()         │
│           │                                                 │
│  ┌────────┴──────────────────────────────┐                  │
│  │      GMF 编码 Pipeline                │  [RECORDING 时]  │
│  │  gmf_mic_io → pcm_enc_el → BLE 发送  │                  │
│  └───────────────────────────────────────┘                  │
│                           │ 按键中断                         │
│                    audio_pipeline_stop()                    │
│                    resume AFE Manager                       │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 状态机

```
          系统启动
              │
              ▼
    ┌─────────────────┐
    │   IDLE          │  AFE Manager 运行（WakeNet 监听）
    │   mic 常开      │◄──────────────────────────────┐
    └────────┬────────┘                               │
             │ WAKEUP_START 事件                       │
             ▼                                        │
    挂起 AFE Manager                                  │
    播放提示音（< 200ms）                              │
             │                                        │
             ▼                                        │
    ┌─────────────────┐                               │
    │   RECORDING     │  GMF Pipeline 运行             │
    │   Opus + BLE    │                               │
    └────────┬────────┘                               │
             │ 按键中断 / BLE 断开                     │
             ▼                                        │
    audio_pipeline_stop()                             │
    发送 FRAME_TYPE_RECORD_END (0x12)                  │
    恢复 AFE Manager ───────────────────────────────►─┘
```

### 3.3 I2S 资源调度

| 阶段 | I2S 使用者 | 说明 |
|------|-----------|------|
| IDLE | AFE Manager（via `read_cb`） | mic driver 常开，feed_task 轮询读取 |
| IDLE→RECORDING 切换 | — | suspend AFE Manager → mic driver 仍开，GMF mic_io 接管读取 |
| RECORDING | GMF mic_io | GMF Pipeline 通过 mic_io 读取 I2S |
| RECORDING→IDLE 切换 | — | `audio_pipeline_stop()` 销毁 Pipeline → resume AFE Manager 重新读取 |

> **关键**：mic driver（I2S）始终保持初始化状态；AFE Manager 和 GMF Pipeline 通过 suspend/resume 交替持有读取权，不会同时读取。

### 3.4 新增文件结构

```
firmware/main/
├── wake_detector.c / wake_detector.h   # AFE Manager 封装，唤醒回调，suspend/resume
├── button_handler.c / button_handler.h # BOOT 键 GPIO 中断，停止录音
└── audio/
    └── audio_driver.h                  # 新增：mic_driver_start_always() 接口（保持 I2S 常开）
```

### 3.5 依赖组件（新增至 `idf_component.yml`）

```yaml
dependencies:
  espressif/gmf_ai_audio:
    version: ">=0.7.0"      # 提供 esp_gmf_afe_manager, esp_gmf_afe
  espressif/esp-sr:
    version: ">=2.0.0"      # 提供 WakeNet9 模型
```

---

## 4. 逻辑设计

### 4.1 核心流程

**系统初始化流程**：

```
app_main()
    │
    ├── audio_driver_es7210_register()
    ├── audio_pipeline_init()           // 原有：初始化 GMF Pool（mic 常开）
    ├── wake_detector_init()            // 新增：初始化 AFE Manager，启动监听
    ├── button_handler_init()           // 新增：注册 BOOT 键 GPIO 中断
    └── nimble_port_freertos_init()
```

**唤醒 → 录音流程**（AFE Manager 回调线程）：

```
ESP_GMF_AFE_EVT_WAKEUP_START 回调触发
    │
    ├── 检查 audio_pipeline_get_state() == AUDIO_STATE_IDLE？
    │     否 → 忽略（防抖）
    │
    ├── wake_detector_suspend()         // 挂起 AFE，让出 I2S 读取权
    │
    ├── audio_player_play_tone()        // 播放提示音（可选，< 200ms）
    │
    └── audio_pipeline_start()          // 启动 GMF 编码 Pipeline
```

**按键停止录音流程**（GPIO ISR → 延迟任务）：

```
BOOT 键中断触发
    │
    ├── 检查 audio_pipeline_get_state() == AUDIO_STATE_RECORDING？
    │     否 → 忽略
    │
    ├── audio_pipeline_stop()           // 停止 GMF Pipeline，发送 0x12 结束帧
    │
    └── wake_detector_resume()          // 恢复 AFE Manager 监听
```

### 4.2 AFE Manager 配置（WakeNet-only 模式）

```
afe_config_t:
  - input_format = "M"        // 单麦克风，无参考信号
  - wakenet_enable = true
  - vad_enable = false         // 本期不启用 VAD（按键手动结束）
  - WakeNet 模型 = wn9_jarvis_tts

esp_gmf_afe_manager_cfg_t:
  - read_cb = wake_detector_read_cb   // 从 mic driver 读 I2S 数据
  - result_cb = wake_detector_result_cb  // 处理 fetch 结果（触发 WAKEUP_START）
```

### 4.3 业务规则

- AFE Manager 在 RECORDING 状态下必须挂起，不得与 GMF mic_io 同时读取 I2S
- RECORDING 状态下再次检测到唤醒词时忽略（防抖保护）
- BLE 断开时若处于 RECORDING：调用 `audio_pipeline_stop()`，然后 `wake_detector_resume()`
- 按键 ISR 中不直接调用 Pipeline 接口，通过 FreeRTOS 任务通知延迟处理

### 4.4 边界/异常处理

| 场景 | 处理方式 |
|------|---------|
| AFE Manager 初始化失败 | ERROR 日志，系统仅支持 BLE 远程控制录音，不支持唤醒词 |
| WakeNet 模型分区未找到 | ERROR 日志，降级为无唤醒词模式 |
| BLE 断开（录音中） | `audio_pipeline_stop()` → `wake_detector_resume()`，回到 IDLE 监听 |
| 按键在 IDLE 状态下被按下 | 忽略，状态机检查保护 |
| I2S 读取超时（AFE read_cb） | 返回 0，跳过当前帧，继续下一轮 |
| 提示音资源不可用 | 跳过提示音，直接 `audio_pipeline_start()` |

---

## 5. 测试方案

### 5.1 测试策略

| 层级 | 范围 | Mock 边界 |
|------|------|----------|
| 单元测试 | 状态机转换逻辑（IDLE→RECORDING→IDLE）| Mock AFE Manager 回调、Mock Pipeline start/stop |
| 单元测试 | 按键防抖逻辑（IDLE 时按键忽略） | Mock GPIO 中断、Mock Pipeline state |
| 集成验证（手动） | 实机说 "Jarvis" 触发录音、按键停止 | 无 Mock，全链路验证 |
| 功能验证 | 时延测量（唤醒响应 ≤ 500ms，按键响应 ≤ 200ms） | 串口日志时间戳分析 |

### 5.2 关键用例

| 用例 | 输入 | 期望结果 |
|------|------|---------|
| 正常唤醒 | 说 "Jarvis" | ≤ 500ms 内播放提示音并开始 BLE 发送 |
| 按键停止 | 录音中按下 BOOT 键 | ≤ 200ms 内停止发送，发送 `FRAME_TYPE_RECORD_END (0x12)` |
| IDLE 时按键无效 | IDLE 状态下按下 BOOT 键 | 不触发任何状态变化 |
| 噪声不触发 | 普通环境噪声 | 不进入 RECORDING 状态 |
| BLE 断开恢复 | 录音中 BLE 断开后重连 | 自动回到 IDLE，重连后可正常再次唤醒 |
| 连续唤醒防抖 | RECORDING 中再次说 "Jarvis" | 忽略，保持 RECORDING |

---

## 6. 影响评估

### 6.1 对现有功能的影响

- `audio_pipeline.h` / `audio_pipeline.c`：**不修改**，对外接口完全兼容
- `main.c`：新增 `wake_detector_init()` 和 `button_handler_init()` 调用
- iOS 远程控制（`CMD_START_RECORD / CMD_STOP_RECORD`）：**保持有效**，两条控制路径并存（本地唤醒词 + iOS 远程），互不冲突

### 6.2 对其他模块的影响

- **ble-channel**：无需修改，结束帧类型沿用 `FRAME_TYPE_RECORD_END (0x12)`
- **pipecat-pipeline**：无影响，音频数据格式不变

### 6.3 新增组件依赖

- `espressif/gmf_ai_audio >= 0.7.0`
- `espressif/esp-sr >= 2.0.0`（含 `wn9_jarvis_tts` 模型分区）

### 6.4 回滚方案

- 删除 `wake_detector_init()` 和 `button_handler_init()` 调用，系统退回纯 BLE 远程控制模式
- `sdkconfig` 中关闭 WakeNet 相关模型分区，节省 Flash 空间
