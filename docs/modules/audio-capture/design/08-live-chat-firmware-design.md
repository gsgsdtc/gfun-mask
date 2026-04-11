# Design: 08 - Live 聊天固件层（VADNet + 状态机）

> 所属模块：audio-capture
> 关联需求：docs/feat/feat-08-live-chat-wake-word-vad.md
> 关联 iOS 设计：docs/modules/voice-chat/design/08-live-chat-ios-design.md
> 关联后端设计：docs/modules/pipecat-pipeline/design/08-live-chat-backend-design.md
> 更新日期：2026-04-08
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

在 ESP32-S3 固件中启用 AFE VADNet，实现"Jarvis 唤醒 → VAD 自动检测句末 → BLE 发送音频 → 回到监听"的闭环，替换 feat-07 的按键停止逻辑。

### 1.2 设计约束

- VADNet 与 WakeNet9 共享同一 AFE Manager，通过 `vad_init = true` 启用
- AFE Manager 与 GMF 编码 Pipeline 仍通过 suspend/resume 交替使用 I2S
- 新增两种 BLE L2CAP 控制帧，不修改现有 `0x01` 音频帧结构
- 废弃 feat-07 按键停止分支（`button_handler.c` 中的 `audio_pipeline_stop` 调用）
- 不涉及 ESP32 硬件深度睡眠

---

## 2. 接口设计

### 2.1 BLE L2CAP 帧类型变更

| 帧类型 | 字节值 | 说明 | 变更 |
|--------|--------|------|------|
| 音频帧 | `0x01` | Opus 编码音频数据 | 不变 |
| 语音开始 | `0x03` | VADNet 检测到用户开始说话 | **新增** |
| 语音结束 | `0xFE` | VADNet 检测到句末，本轮音频结束 | 原 `FRAME_TYPE_RECORD_END 0x12`，调整为 `0xFE` 与协议规划对齐 |

### 2.2 内部接口变更

| 函数/方法 | 变更类型 | 说明 |
|-----------|---------|------|
| `wake_detector_init()` | **修改** | `vad_init` 从 `false` 改为 `true`；在 `_afe_result_cb` 中增加 VAD 事件处理 |
| `wake_detector_suspend()` | 不变 | 唤醒词检测挂起 |
| `wake_detector_resume()` | 不变 | 唤醒词检测恢复 |
| `audio_pipeline_stop_vad()` | **新增** | 由 VAD 结束触发的停止：发送 `0xFE` 帧 → `audio_pipeline_stop()` → `wake_detector_resume()` |
| `button_handler` 中的停止调用 | **废弃** | 移除按键触发 `audio_pipeline_stop` 的逻辑 |

### 2.3 AFE 结果回调新增事件

| `wakeup_state` 枚举值 | 原有处理 | feat-08 新增处理 |
|-----------------------|---------|-----------------|
| `WAKENET_DETECTED` | 挂起 AFE → 启动 Pipeline | 不变 |
| `VAD_START` | 无 | 发送 `0x03` 语音开始帧（用于 iOS 打断 TTS） |
| `VAD_END` | 无 | 调用 `audio_pipeline_stop_vad()` |

---

## 3. 状态机设计

### 3.1 固件会话状态

```
                    ┌─────────────────────────────────────────┐
                    │                                         │
    上电 ──────► IDLE ◄──── wake_detector_resume() ◄──── VAD_END
                    │                                         │
          WAKENET_DETECTED                         audio_pipeline_stop_vad()
                    │                                         │
                    ▼                                         │
               RECORDING ──────── VAD_START ──────► [发送 0x03 帧]
               (GMF Pipeline                        [继续录音]
                运行中)
                    │
                 VAD_END
                    │
                    ▼
          发送 0xFE 帧 + 停止 Pipeline
```

### 3.2 状态说明

| 状态 | I2S 持有者 | AFE Manager | GMF Pipeline |
|------|-----------|-------------|-------------|
| IDLE | AFE Manager | 运行（WakeNet + VADNet 监听） | 停止 |
| RECORDING | GMF Pipeline | 挂起 | 运行（采集 + Opus 编码 + BLE 发送） |

---

## 4. 逻辑设计

### 4.1 核心流程：唤醒 → VAD → 停止

```
AFE Manager (WakeNet+VADNet)        GMF Pipeline          BLE L2CAP
        │                                │                     │
  检测到 "Jarvis"                        │                     │
        │                                │                     │
  wake_detector_suspend()               │                     │
        │                                │                     │
  audio_pipeline_start() ──────────────►│                     │
        │                         开始采集 Opus 编码            │
        │                                │──── 0x01 音频帧 ──►│
  VAD_START 事件                         │                     │
        │──────────────────────────────────── 0x03 语音开始 ──►│
        │                                │                     │
  VAD_END 事件                           │                     │
        │                                │                     │
  audio_pipeline_stop_vad()             │                     │
        │──────────────────────────────────── 0xFE 语音结束 ──►│
        │                         停止 Pipeline                │
  wake_detector_resume()                │                     │
        │                                │                     │
  继续监听 "Jarvis"                      │                     │
```

### 4.2 业务规则

- VAD_START 帧仅在 RECORDING 状态中发送，IDLE 状态下的 VAD 事件忽略
- VAD_END 触发后，立即停止 Pipeline（不等待 BLE 发送队列清空），末尾帧后跟 `0xFE`
- 若 Pipeline 启动失败，立即调用 `wake_detector_resume()`，不发送任何 VAD 帧
- 按键停止（feat-07）逻辑在本 feat 中废弃，按键可保留作为紧急重置功能（可选）

### 4.3 异常处理

| 异常 | 处理方式 |
|------|---------|
| VAD_END 在 IDLE 状态触发 | 忽略，打印 WARNING |
| BLE 未连接时发送帧失败 | 忽略，继续 `wake_detector_resume()` |
| audio_pipeline_start 失败 | 立即 `wake_detector_resume()`，不发 VAD 帧 |
| VADNet 误判（过短语音触发 VAD_END） | 依赖 AFE `vad_min_speech_ms` 参数过滤，建议 ≥ 300ms |

---

## 5. 测试方案

### 5.1 测试策略

| 层级 | 范围 | 方式 |
|------|------|------|
| 集成测试 | 唤醒 → VAD → BLE 帧序列 | 串口监视 + iOS App 抓帧 |
| 手动测试 | VAD 误判率（短词/咳嗽/噪音） | 实机多轮验证 |
| 回归测试 | 确认按键唤醒路径已废弃 | 确认按键不再触发 pipeline_stop |

### 5.2 关键用例

| 用例 | 操作 | 期望 BLE 帧序列 |
|------|------|----------------|
| 正常对话 | 说 "Jarvis"，说一句话，停顿 | `0x01...` → `0x03` → `0x01...` → `0xFE` |
| 打断场景 | 说 "Jarvis"，说话，VAD 误触发后继续说 | `0x03` → `0x01...` → `0x03`（再次触发）→ ... → `0xFE` |
| 快速唤醒 | 连续两次说 "Jarvis" | 第二次唤醒在 IDLE 状态被正确处理 |

---

## 6. 影响评估

### 6.1 对现有功能的影响

- **feat-07 按键录音**：按键停止录音逻辑废弃，按键可保留作为调试重置
- **audio_pipeline.c**：接口不变，新增 `audio_pipeline_stop_vad()` 包装函数

### 6.2 回滚方案

- 将 `wake_detector.c` 中 `vad_init` 改回 `false`
- 恢复 `button_handler.c` 中的 `audio_pipeline_stop` 调用
- 无需修改 BLE 协议（iOS 侧忽略未知帧类型即可兼容）
