# Module Spec: voice-chat

> 模块：iOS 语音聊天客户端
> 最近同步：2026-03-12
> 状态：Phase 3 完成（iOS 语音聊天全链路验证通过）

---

## 1. 模块概述

iOS 端语音聊天模块，通过 WebSocket 与 Pipecat 后端通信，实现完整的语音对话体验。支持两种输入模式：
- **BLE 模式**：接收来自 ESP32 设备的 BLE L2CAP 音频数据，转发至 Pipecat 服务端
- **本机录音模式**（预留）：使用 iPhone 麦克风直接录音

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | BLEManager（BLE L2CAP 音频帧）或本机麦克风 |
| 下游 | Pipecat WebSocket 服务端（`ws://<host>:8765/ws`） |
| 输入 | BLE 音频帧（PCM，16kHz，16-bit，单声道）、用户 UI 操作 |
| 输出 | TTS 音频播放、聊天气泡（用户转录文本 + AI 回复文本） |

### 1.2 文件结构

```
VoiceMaskApp/VoiceMaskApp/VoiceChat/
├── ChatMessage.swift           # 消息数据模型
├── PipecatWebSocketClient.swift # WebSocket 客户端，处理协议收发与断线重连
├── TtsAudioPlayer.swift        # TTS MP3 音频缓冲与播放
├── VoiceChatView.swift         # SwiftUI 聊天 UI（气泡列表 + 录音按钮）
└── VoiceChatViewModel.swift    # ViewModel：状态管理、BLE 数据桥接、业务逻辑
```

---

## 2. 数据模型

### 2.1 ChatMessage

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | `UUID` | 唯一标识 |
| `role` | `ChatMessage.Role` | `.user` 或 `.assistant` |
| `text` | `String` | 消息文本内容 |
| `timestamp` | `Date` | 创建时间 |

### 2.2 ChatState（状态枚举）

| 状态 | 说明 |
|------|------|
| `.idle` | 空闲，等待操作 |
| `.recording` | 录音中（累积 PCM 帧） |
| `.processing` | 处理中（STT → LLM → TTS） |
| `.playing` | TTS 播放中 |
| `.error(String)` | 错误状态，携带错误描述 |

### 2.3 PipecatEvent（WebSocket 事件枚举）

| 事件 | 说明 | 来源消息 |
|------|------|---------|
| `.connected` | WebSocket 连接建立 | — |
| `.disconnected` | 连接断开 | — |
| `.ready` | 服务端就绪 | `{"type":"ready"}` |
| `.transcriptFinal(String)` | STT 识别结果 | `{"type":"transcript_final","text":"..."}` |
| `.llmDone(String)` | LLM 完整回复 | `{"type":"llm_done","text":"..."}` |
| `.ttsStart` | TTS 开始 | `{"type":"tts_start"}` |
| `.ttsAudio(Data)` | TTS 音频块 | 二进制（首字节 `0xAA`） |
| `.ttsEnd` | TTS 结束 | `{"type":"tts_end"}` |

---

## 3. 对外接口

### 3.1 VoiceChatViewModel（供 ContentView 调用）

| 方法/属性 | 类型 | 说明 |
|----------|------|------|
| `chatState` | `@Published ChatState` | 当前状态 |
| `messages` | `@Published [ChatMessage]` | 聊天历史消息列表 |
| `isWebSocketConnected` | `@Published Bool` | WebSocket 连接状态 |
| `recordingDuration` | `@Published TimeInterval` | 当前录音时长（秒） |
| `serverURL` | `@Published String` | 服务器地址（持久化至 UserDefaults） |
| `connectWebSocket()` | `func` | 建立 WebSocket 连接 |
| `disconnectWebSocket()` | `func` | 断开 WebSocket 连接 |
| `startRecording()` | `func` | 开始录音（发送 `start` 指令） |
| `stopRecording()` | `func` | 停止录音（发送 `stop` 指令） |
| `handleBLEAudioFrame(_ data: Data)` | `async func` | 转发 BLE 音频帧至 WebSocket |
| `clearMessages()` | `func` | 清空聊天记录 |

### 3.2 PipecatWebSocketClient（供 VoiceChatViewModel 调用）

| 方法 | 说明 |
|------|------|
| `connect(to urlString: String)` | 连接 WebSocket，失败自动退避重连 |
| `disconnect()` | 主动断开（不触发重连） |
| `startRecording()` | 发送 `{"type":"start"}` |
| `stopRecording()` | 发送 `{"type":"stop"}` |
| `sendAudioFrame(_ data: Data)` | 发送二进制 PCM 帧 |

### 3.3 TtsAudioPlayer（供 VoiceChatViewModel 调用）

| 方法 | 说明 |
|------|------|
| `appendAudio(_ data: Data)` | 追加 MP3 数据到缓冲区 |
| `playBuffered()` | 将已缓冲 MP3 写入临时文件并播放 |
| `reset()` | 清空缓冲区，停止播放 |

---

## 4. 状态机

```
[idle]
  │ connectWebSocket() → WS 建立
  ▼
[idle / isWebSocketConnected=true]
  │ startRecording()
  ▼
[recording]         ── BLE 音频帧持续转发
  │ stopRecording()
  ▼
[processing]        ── 等待 transcript_final → llm_done → tts_start
  │ ttsStart 事件
  ▼
[playing]           ── 累积 MP3 帧，ttsEnd 后播放
  │ 播放完成（AVAudioPlayerDelegate）
  ▼
[idle]

任意状态 ──────────── 15s 超时 ──────────►  [idle]
任意状态 ──────────── WS 断开 ────────────► [idle]（触发自动重连）
```

---

## 5. 关键逻辑

### 5.1 BLE 音频桥接

```
BLEManager（ContentView 回调）
  │ data: Data（PCM 帧）
  ▼
VoiceChatViewModel.handleBLEAudioFrame(_:)
  │ chatState == .recording 时
  ▼
PipecatWebSocketClient.sendAudioFrame(_:)
  │ 发送二进制 WebSocket 帧
  ▼
Pipecat 服务端
```

### 5.2 断线自动重连

- `intentionalDisconnect = false` 时，意外断连触发退避重连
- 退避间隔：初始 1s，每次失败翻倍，最大 30s
- 连接成功后重置退避间隔为 1s

### 5.3 TTS 播放流程

```
tts_start 事件 → TtsAudioPlayer.reset()（清空旧缓冲）
ttsAudio 事件 → TtsAudioPlayer.appendAudio()（追加 MP3 分块）
tts_end 事件  → TtsAudioPlayer.playBuffered()（合并播放）
播放完成      → chatState = .idle
```

### 5.4 处理超时保护

- `stopRecording()` 触发后启动 15s 超时计时器
- 超时后若 `chatState` 仍为 `.processing` 或 `.playing`，强制重置为 `.idle`
- 收到 `tts_end` + 播放完成后取消计时器

---

## 6. 配置参数

| 参数 | 默认值 | 持久化 | 说明 |
|------|--------|--------|------|
| `serverURL` | `ws://192.168.50.125:8765/ws` | UserDefaults | Pipecat 服务器地址 |
| 处理超时 | `15s` | 代码常量 | STT/LLM/TTS 全链路超时阈值 |
| 心跳间隔 | `30s` | 代码常量 | ping 发送周期 |
| pong 超时 | `10s` | 代码常量 | 超时则触发重连 |
| 最大重连间隔 | `30s` | 代码常量 | 退避重连上限 |

---

## 7. 验收状态

| 验收项 | 状态 |
|--------|------|
| WebSocket 连接建立，收到 `ready` 后状态正常 | ✅ |
| 点击开始/停止录音，BLE 音频正确转发 | ✅ |
| STT 识别文本展示为用户气泡 | ✅ |
| LLM 回复文本展示为 AI 气泡 | ✅ |
| TTS MP3 音频播放正常 | ✅ |
| 意外断连后自动退避重连 | ✅ |
| 15s 超时后状态自动重置 | ✅ |
| 服务器地址持久化（重启后保留） | ✅ |

---

## 8. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-12 | feat #03 | 初始实现：VoiceChatView + ViewModel + PipecatWebSocketClient + TtsAudioPlayer |
| 2026-03-12 | fix | 修复聊天气泡未正常显示问题，messages 绑定与状态更新逻辑修正 |
