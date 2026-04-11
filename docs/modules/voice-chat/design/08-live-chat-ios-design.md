# Design: 08 - Live 聊天 iOS 层（状态机 + 打断 + 浅休眠）

> 所属模块：voice-chat
> 关联需求：docs/feat/feat-08-live-chat-wake-word-vad.md
> 关联固件设计：docs/modules/audio-capture/design/08-live-chat-firmware-design.md
> 关联后端设计：docs/modules/pipecat-pipeline/design/08-live-chat-backend-design.md
> 目标平台：iOS 16+，SwiftUI
> 更新日期：2026-04-08
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

在 iOS App 中实现 Live 聊天状态机：接收 ESP32 的 BLE 控制帧（唤醒/VAD 开始/VAD 结束），驱动 WebSocket 与 Pipecat 交互，处理 TTS 打断，并在 30 秒空闲后进入浅休眠。

### 1.2 设计约束

- 复用现有 `BLEManager` / `L2CAPHandler`，不修改 BLE 连接逻辑
- WebSocket 连接复用 feat-03 的 `PipecatService`，新增消息类型
- 浅休眠不断开 BLE，仅停止 WebSocket 音频转发和 TTS 播放
- 打断检测基于 ESP32 的 `0x03` 语音开始帧，iOS 不做独立 VAD

---

## 2. 状态机设计

### 2.1 会话状态定义

```swift
enum LiveChatState {
    case sleep          // 休眠：WebSocket 已断开，iOS 释放后台网络资源（5min 空闲触发）
    case listening      // 监听中：已唤醒，等待用户说话（VAD 运行中）
    case processing     // 处理中：VAD 结束帧已收到，ASR→LLM 进行中
    case ttsPlaying     // 播放中：TTS 音频正在播放
}
```

### 2.2 状态转换图

```
         BLE 0x01/0x03（重建 WebSocket）
    ┌──────────────────────────────────────────┐
    │                                          │
    ▼                                          │
  SLEEP ──────────────────────────► LISTENING ─┘
    ▲                                    │    │
    │    5min 无活动                      │    │ 收到 0x03（VAD_START）
    └────────────────────────────────────┘    │ [TTS_PLAYING 时：打断]
                                              │ 收到 0xFE（VAD_END）
                                              ▼
                                         PROCESSING
                                              │
                                       收到 tts_start
                                              ▼
                                         TTS_PLAYING
                                              │
                                       收到 tts_end
                                              ▼
                                          LISTENING
```

### 2.3 状态对应行为

| 状态 | BLE 音频 | WebSocket | TTS 播放 | 空闲计时 |
|------|---------|-----------|---------|---------|
| SLEEP | 忽略（仅监控唤醒帧） | **已断开** | 停止 | 暂停 |
| LISTENING | 转发至 WebSocket | 转发音频帧 | 停止 | 运行（5min 计时） |
| PROCESSING | 忽略（VAD 已结束） | 等待 llm/tts 消息 | 停止 | 运行 |
| TTS_PLAYING | 监控 `0x03` 帧 | 等待 tts_end | 播放 | 运行 |

---

## 3. 接口设计

### 3.1 BLE 帧处理变更

| 帧类型 | 字节值 | feat-07 处理 | feat-08 处理 |
|--------|--------|-------------|-------------|
| 音频帧 | `0x01` | 存储/上传 | 转发至 WebSocket（LISTENING 状态） |
| 语音开始 | `0x03` | 无 | **新增**：TTS_PLAYING → 触发打断；SLEEP → 切换至 LISTENING |
| 语音结束 | `0xFE` | 上传录音 | 发送 `{"type":"stop"}` 至 WebSocket，切换至 PROCESSING |

### 3.2 WebSocket 新增消息类型

**iOS → Pipecat（新增）**

| 消息 | 触发时机 |
|------|---------|
| `{"type":"interrupt"}` | 收到 `0x03` 帧且当前状态为 TTS_PLAYING |
| `{"type":"sleep"}` | 空闲计时器到期，进入 SLEEP |
| `{"type":"wake"}` | 从 SLEEP 唤醒，切换至 LISTENING |

**Pipecat → iOS（复用 feat-03，无新增）**

| 消息 | feat-08 处理 |
|------|-------------|
| `{"type":"tts_start"}` | 切换至 TTS_PLAYING |
| TTS 音频帧（Binary `0xAA` 前缀） | 播放 |
| `{"type":"tts_end"}` | 停止播放，切换至 LISTENING，重置计时器 |

### 3.3 新增组件

| 组件 | 职责 |
|------|------|
| `LiveChatManager` | 状态机主控，持有状态、计时器、打断逻辑 |
| `SleepTimer` | 5 分钟倒计时，到期断开 WebSocket，切换至 SLEEP |

### 3.4 现有组件变更

| 组件 | 变更 |
|------|------|
| `L2CAPHandler` | 新增 `0x03` / `0xFE` 帧类型分发至 `LiveChatManager` |
| `PipecatService` | 新增发送 `interrupt` / `sleep` / `wake` 消息方法 |
| `AudioPlayer` | 新增 `stopImmediate()` 方法（打断用） |

---

## 4. 逻辑设计

### 4.1 打断流程

```
L2CAPHandler          LiveChatManager        AudioPlayer       PipecatService
     │                      │                     │                  │
收到 0x03 帧                │                     │                  │
     │──── frameReceived ──►│                     │                  │
     │               state == .ttsPlaying?        │                  │
     │                      │── Yes               │                  │
     │                      │──── stopImmediate ─►│                  │
     │                      │                  停止播放               │
     │                      │──────────────────────── interrupt ─────►│
     │                      │                                    取消 LLM/TTS
     │               state = .listening            │                  │
     │                      │                     │                  │
     │               重置空闲计时器                 │                  │
```

### 4.2 休眠流程

```
SleepTimer            LiveChatManager        PipecatService       WebSocket
    │                      │                     │                    │
5min 无活动                 │                     │                    │
    │──── timerFired ──────►│                     │                    │
    │               state = .sleep               │                    │
    │                      │──── sleep ─────────►│                    │
    │                      │──────────────────────────── disconnect ──►│
    │                                            │               连接断开
    │
    (等待唤醒)
    │
收到 BLE 0x01/0x03 帧        │
    │──── frameReceived ──►│
    │               重建 WebSocket ──────────────────────────────────────►│
    │               state = .listening           │               连接建立
    │                      │──── wake ──────────►│
    │               重置计时器                    │
```

### 4.3 业务规则

- `0x03` 语音开始帧在 LISTENING 状态下仅重置计时器，不触发打断
- TTS 打断后，丢弃当前 processing 状态的缓冲音频，等待新的 `stop` 帧
- SLEEP 状态下收到 `0xFE`：忽略（视为误触发）
- 5min 计时器重置事件：收到 `0x01` 音频帧、收到 `tts_start`、收到 `tts_end`
- 唤醒后重建 WebSocket，LLM 上下文清空，开启全新对话

### 4.4 异常处理

| 异常 | 处理方式 |
|------|---------|
| WebSocket 断连（LISTENING 状态） | 尝试重连，期间缓存音频帧；超过 5s 进入 SLEEP |
| `0xFE` 在 SLEEP 状态到达 | 忽略 |
| TTS 音频播放中 BLE 断开 | 继续播放当前缓冲，播完后进入 SLEEP |

---

## 5. UI 变更

### 5.1 状态指示（VoiceChatView）

在现有页面底部状态栏新增 Live Chat 状态显示：

```
┌─────────────────────────────────────┐
│                                     │
│  [对话气泡区，不变]                   │
│                                     │
├─────────────────────────────────────┤
│  😴 浅休眠，说 "Jarvis" 唤醒         │  ← SLEEP
│  👂 正在监听...                      │  ← LISTENING
│  ⚙️  处理中...                       │  ← PROCESSING
│  🔊 播放回复中（点击打断）            │  ← TTS_PLAYING
└─────────────────────────────────────┘
```

状态文字替换现有的"收音中..."/"停止对话"按钮逻辑。

---

## 6. 测试方案

### 6.1 测试策略

| 层级 | 范围 | Mock 边界 |
|------|------|----------|
| 单元测试 | `LiveChatManager` 状态机转换 | Mock L2CAPHandler、PipecatService |
| 单元测试 | `IdleTimer` 计时与重置 | Mock 系统时间 |
| 集成测试 | BLE 帧 → 状态转换 → WebSocket 消息 | Mock WebSocket |
| 手动 E2E | 完整对话 + 打断 + 浅休眠 | 实机 + 实际 Pipecat |

### 6.2 关键用例

| 用例 | 输入序列 | 期望状态序列 |
|------|---------|------------|
| 正常对话 | `0x01帧` → `0xFE` → `tts_start` → `tts_end` | SLEEP→LISTENING→PROCESSING→TTS_PLAYING→LISTENING |
| 打断 TTS | TTS_PLAYING 中收到 `0x03` | TTS_PLAYING→LISTENING，WebSocket 发 interrupt |
| 休眠 | LISTENING 5min 无帧 | LISTENING→SLEEP，WebSocket 发 sleep 后断开 |
| 休眠唤醒 | SLEEP 中收到 `0x01` | SLEEP→LISTENING，重建 WebSocket，发 wake |

---

## 7. 影响评估

### 7.1 对现有功能的影响

- **feat-03 VoiceChatView**：状态文字和按钮逻辑修改，气泡对话区不变
- **feat-07 按键录音页面**：不受影响（不同 Tab）

### 7.2 回滚方案

- `LiveChatManager` 独立模块，可整体禁用，回退至 feat-03 手动按键录音流程
