# Frontend Design: 03-iOS 语音聊天前端

> 所属模块：voice-chat
> 关联需求：docs/feat/feat-03-ios-voice-chat-pipecat.md
> 关联后端设计：docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md
> 目标平台：iOS（SwiftUI，iOS 16+）
> 更新日期：2026-03-10
> 状态：草稿

## 1. 设计概述

### 1.1 目标

在现有 VoiceMaskApp 的 TabView 中新增"语音聊天"标签页，复用 BLE 音频流，通过 WebSocket 实时将 PCM 音频推送至 Pipecat 后端，并将 STT 转写、LLM 回复以气泡对话形式展示，TTS 音频通过 iOS 扬声器播放。

### 1.2 设计约束

- 平台：iOS 16+，SwiftUI
- 复用现有 BLEManager / L2CAPHandler（不修改，仅新增 audio 分发路径）
- 新增代码不破坏现有 Tab 1（录音控制）和 Tab 2（录音列表）功能
- Pipecat 服务器地址可在 App 内配置，存入 UserDefaults
- 无障碍：主要交互元素提供 accessibilityLabel
- 性能：WebSocket 音频转发延迟 ≤ 1 帧（20ms）

---

## 2. 页面原型

### 2.1 页面清单

| 页面名称 | Tab/Sheet | 优先级 | 说明 |
|----------|-----------|--------|------|
| VoiceChatView | Tab 3：语音聊天 | P0 | 主对话界面 |
| SettingsView（新增 Pipecat 配置区） | Sheet（从 VoiceChatView 进入） | P1 | 配置服务器地址 |

### 2.2 页面线框图

**Tab 3：VoiceChatView（语音聊天）**

```
┌─────────────────────────────────────┐
│ ● VoiceMask-01   WS: 已连接  ⚙️    │  ← 状态栏（BLE状态 + WebSocket状态 + 设置按钮）
├─────────────────────────────────────┤
│                                     │
│  ┌─────────────────────────────┐    │
│  │ 你好，今天天气怎么样？       │    │  ← 用户气泡（右对齐，蓝色）
│  └─────────────────────────────┘    │
│                                     │
│  ┌──────────────────────────────────┐│
│  │ 今天北京晴，气温18°C，适合外出。  ││  ← AI 气泡（左对齐，灰色）
│  └──────────────────────────────────┘│
│                                     │
│       [处理中...]                   │  ← 状态指示（空时不显示）
│                                     │
├─────────────────────────────────────┤
│  ┌──────────────────────────────┐   │
│  │  🎙 收音中... 2.4s           │   │  ← 状态描述（随状态变化）
│  └──────────────────────────────┘   │
│                                     │
│  ┌──────────────────────────────┐   │
│  │         ● 停止对话           │   │  ← 主操作按钮（录音中：红色●，待机：绿色▶）
│  └──────────────────────────────┘   │
│                                     │
└─────────────────────────────────────┘
  🎙 录音控制    📋 录音列表    💬 语音聊天   ← TabBar
```

**Sheet：Pipecat 服务器设置**

```
┌─────────────────────────────────────┐
│  Pipecat 服务器配置           [完成]│
├─────────────────────────────────────┤
│                                     │
│  服务器地址                         │
│  ┌──────────────────────────────┐   │
│  │ ws://192.168.1.100:8765/ws   │   │
│  └──────────────────────────────┘   │
│  示例：ws://192.168.x.x:8765/ws    │
│                                     │
│  [测试连接]  → 成功 ✅ / 失败 ❌   │
│                                     │
└─────────────────────────────────────┘
```

### 2.3 页面交互关系

**状态流转（VoiceChatView 内）**

```
[待命] ──点击"开始对话"──▶ [收音中] ──点击"停止/发送"──▶ [处理中]
  ▲                                                           │
  └──────────────────── TTS 播放完毕 ◀──── [播放回复] ◀──────┘
  ▲
  └── BLE 未连接 / WS 未连接 → 按钮禁用，提示用户先连接
```

**页面数据传递**

| 来源 | 目标 | 触发 | 传递数据 | 方式 |
|------|------|------|----------|------|
| VoiceChatView | SettingsSheet | 点击 ⚙️ | 无 | `.sheet` |
| SettingsSheet | VoiceChatView | 点击"完成" | serverURL(String) | UserDefaults |

**入口与出口条件**

| 页面 | 入口条件 | 出口条件 | 异常处理 |
|------|----------|----------|----------|
| VoiceChatView | 用户切换到 Tab 3 | 切换到其他 Tab | WS 断连→显示提示，保留对话历史 |
| SettingsSheet | 点击 ⚙️ 按钮 | 点击"完成" | 地址格式错误→红色提示 |

---

## 3. 组件层次结构

### 3.1 组件树

```
ContentView（现有，TabView）
├── Tab 1: RecordingControlView（现有，不变）
├── Tab 2: RecordingListView（现有，不变）
└── Tab 3: VoiceChatView（新增）
    ├── VoiceChatStatusBar              ← BLE + WS 状态指示
    ├── ChatMessageList                 ← ScrollView + 气泡列表
    │   └── ChatBubbleView（×N）        ← 单条消息气泡
    ├── VoiceChatStatusLabel            ← "收音中 2.4s" / "处理中..." / "播放回复中"
    ├── VoiceChatActionButton           ← 开始/停止主按钮
    └── PipecatSettingsSheet（Sheet）
        ├── ServerURLField
        └── TestConnectionButton
```

### 3.2 共享组件

| 组件名 | 职责 | 复用范围 |
|--------|------|----------|
| 无（本 feat 组件均为专属） | — | — |

### 3.3 页面专属组件

| 所属页面 | 组件名 | 职责 | 状态需求 |
|----------|--------|------|----------|
| VoiceChatView | VoiceChatStatusBar | 显示 BLE/WS 双连接状态 | 读取 ViewModel |
| VoiceChatView | ChatMessageList | 气泡列表，自动滚动到底部 | 读取 ViewModel.messages |
| VoiceChatView | ChatBubbleView | 单条消息（用户/AI，左右对齐） | role: user/assistant |
| VoiceChatView | VoiceChatStatusLabel | 当前状态文字描述 | 读取 ViewModel.chatState |
| VoiceChatView | VoiceChatActionButton | 开始/停止，颜色随状态变 | 读取 ViewModel.chatState |
| SettingsSheet | ServerURLField | WebSocket URL 输入框 + 校验 | 本地 @State |
| SettingsSheet | TestConnectionButton | 测试 WebSocket 连接 | 本地 @State |

---

## 4. 状态管理设计

### 4.1 状态分层

| 状态类型 | 数据 | 管理方式 | 说明 |
|----------|------|----------|------|
| 全局持久化 | `pipecatServerURL: String` | UserDefaults | 服务器地址跨启动保留 |
| 会话状态 | `chatState: ChatState` | VoiceChatViewModel (@ObservableObject) | 当前对话状态枚举 |
| 会话状态 | `messages: [ChatMessage]` | VoiceChatViewModel | 当前会话对话历史（内存，不持久化） |
| 会话状态 | `isWebSocketConnected: Bool` | VoiceChatViewModel | WS 连接状态 |
| UI 状态 | `showSettings: Bool` | VoiceChatView @State | Settings Sheet 是否显示 |
| UI 状态 | `recordingDuration: TimeInterval` | VoiceChatViewModel | 本次收音计时 |

### 4.2 ChatState 枚举

```
enum ChatState {
    case idle           // 待命，等待用户开始
    case recording      // 收音中（BLE → WS 转发）
    case processing     // 等待 STT/LLM/TTS 处理
    case playing        // TTS 音频播放中
    case error(String)  // 错误（附带错误描述）
}
```

### 4.3 ChatMessage 结构

```
struct ChatMessage: Identifiable {
    let id: UUID
    let role: Role      // user | assistant
    let text: String
    let timestamp: Date

    enum Role { case user, assistant }
}
```

### 4.4 状态流转图

```
用户点击"开始"
    → chatState = .recording
    → 向 ESP32 发 START_RECORD
    → WebSocket 发 {"type":"start"}
    → 启动计时器（recordingDuration）

BLE 收到音频帧
    → WebSocket 发送 Binary PCM 帧

用户点击"停止"
    → 向 ESP32 发 STOP_RECORD
    → WebSocket 发 {"type":"stop"}
    → chatState = .processing

WS 收到 transcript_final
    → messages.append(ChatMessage(role:.user, text:...))

WS 收到 llm_done
    → messages.append(ChatMessage(role:.assistant, text:...))

WS 收到 tts_start
    → chatState = .playing

WS 收到 TTS Binary 帧
    → 写入 AVAudioPlayer 缓冲区播放

WS 收到 tts_end
    → chatState = .idle
```

---

## 5. 数据获取策略

### 5.1 WebSocket 依赖（替代传统 API）

| 事件 | 方向 | 触发时机 | 处理方式 |
|------|------|----------|----------|
| 连接建立 | → 服务端 | App 进入 Tab 3 时自动连接 | 收到 ready 事件后标记 isWebSocketConnected=true |
| 音频推送 | → 服务端 | chatState=.recording 期间，每个 BLE 音频帧 | PipecatWebSocketClient.sendAudio(_ data: Data) |
| 控制消息 | → 服务端 | 用户点击开始/停止 | PipecatWebSocketClient.sendControl(type: String) |
| 事件接收 | ← 服务端 | 服务端推送 | PipecatWebSocketClient.onEvent 回调 → ViewModel |

### 5.2 加载/空/错误状态

| 组件 | 加载态 | 空数据态 | 错误态 |
|------|--------|----------|--------|
| ChatMessageList | 无（无需加载） | "还没有对话，点击开始说话" | — |
| WS 连接 | "正在连接服务器..." | — | "服务器未连接，请检查设置" |
| 对话处理中 | "处理中..." + ProgressView | — | error(message) → Toast 提示 |

### 5.3 PipecatClient 协议层封装

> **设计目标**：将 Pipecat WebSocket 协议的全部实现细节（消息格式、帧前缀、连接管理、心跳、重连）封装在 `PipecatWebSocketClient` 内部，ViewModel 和 View 层零感知协议细节。后续切换到其他 Pipecat transport（如 WebRTC、RTVI）时，仅需替换这一层。

#### 5.3.1 公开 API（ViewModel 可见）

```swift
// 事件枚举（只暴露业务语义，不暴露协议字符串）
enum PipecatEvent {
    case ready
    case transcriptFinal(text: String)
    case llmDone(text: String)
    case ttsStart
    case ttsAudio(data: Data)
    case ttsEnd
    case error(code: String, message: String)
    case pong
}

final class PipecatWebSocketClient {
    // 状态（只读）
    private(set) var isConnected: Bool
    private(set) var isConnecting: Bool

    // 回调
    var onEvent: ((PipecatEvent) -> Void)?
    var onConnectionChange: ((Bool) -> Void)?

    // 连接管理
    func connect(to urlString: String)
    func disconnect()

    // 类型化命令（不暴露字符串协议细节）
    func startRecording()        // → {"type":"start"}
    func stopRecording()         // → {"type":"stop"}
    func sendAudioFrame(_ data: Data)  // → Binary PCM
    // ping 由内部心跳定时器自动发送，外部不需调用
}
```

#### 5.3.2 内部实现职责（外部不可见）

| 职责 | 实现细节 |
|------|---------|
| 消息序列化 | `{"type":"start/stop/ping"}` JSON 构造；PCM 二进制直发 |
| 消息反序列化 | `data[0] == 0xAA` 判断 TTS 音频帧；其余 data 尝试 JSON 解析 |
| 连接状态管理 | `isConnected` 仅在收到 `ready` 事件后置 true，防止握手期间误判 |
| 心跳 Ping | 连接建立后每 **30 秒**发送 `{"type":"ping"}`，收到 `pong` 重置计时器；超时（60s 无响应）主动重连 |
| 断线重连 | WS 层错误（`receiveNext` 失败）触发指数退避重连：1s → 2s → 4s → 8s → 最大 30s；仅在 `networkReady=true` 时重连 |
| URL 规范化 | `http://` → `ws://`，`https://` → `wss://` 自动转换 |

#### 5.3.3 心跳与重连时序

```
连接建立 (ready 收到)
    │
    ├─ 启动 pingTimer (30s 周期)
    │
    │  每 30s
    │   │  发 {"type":"ping"}
    │   │  启动 pongTimeout (30s)
    │   │  ─── 收到 pong ──▶ 取消 pongTimeout，下次计时重置
    │   │  ─── pongTimeout 超时 ──▶ 进入重连流程
    │
    ├─ receiveNext() 失败（网络层断连）
    │   └─▶ 重连流程
    │
重连流程:
    disconnect()
    delay(retryInterval)   // 1s → 2s → 4s → ... → 30s
    connect(to: lastURL)
    retryInterval = min(retryInterval * 2, 30)
    成功 (ready) → retryInterval = 1s，重置 pingTimer
```

#### 5.3.4 ViewModel 调用约定

ViewModel **不得**直接调用 `wsClient.send(.string(...))` 或构造任何 JSON，所有操作必须通过类型化方法：

```
❌  wsClient.sendControl(type: "start")   // 字符串泄漏
✅  wsClient.startRecording()              // 语义明确
```

---

## 6. 模块结构（新增文件）

```
VoiceMaskApp/VoiceMaskApp/
├── VoiceChat/
│   ├── VoiceChatView.swift          # 主对话页面（SwiftUI）
│   ├── VoiceChatViewModel.swift     # @ObservableObject，管理 chatState + messages
│   │                                # 依赖 PipecatWebSocketClient 的类型化 API，不接触协议细节
│   ├── PipecatWebSocketClient.swift # Pipecat 协议封装层（唯一接触协议字符串/帧格式的地方）
│   │                                # 职责：连接管理、消息序列化/反序列化、心跳 Ping、断线重连
│   │                                # 公开：startRecording() / stopRecording() / sendAudioFrame()
│   │                                # 内部：JSON 构造、0xAA 前缀解析、pingTimer、retryInterval
│   ├── ChatMessage.swift            # ChatMessage 数据模型（Identifiable）
│   └── TtsAudioPlayer.swift         # TTS 音频播放（AVAudioPlayer，全缓冲 → tts_end 后触发）
└── ContentView.swift                # 现有，新增 Tab 3 入口（改动最小）
```

**层次关系**：

```
VoiceChatView（SwiftUI）
    │ @ObservedObject
    ▼
VoiceChatViewModel（业务逻辑层）
    │ 类型化方法调用
    ▼
PipecatWebSocketClient（协议封装层）← 唯一知道协议细节的地方
    │ URLSessionWebSocketTask
    ▼
Pipecat Backend（WebSocket /ws）
```

---

## 7. 交互流程

### 7.1 完整对话轮次序列

```
用户操作           VoiceChatViewModel        PipecatWebSocketClient    服务端（Pipecat）
─────────────────────────────────────────────────────────────────────────────────────
App 进入 Tab 3  →  connectWebSocket()      →  connect(to: url)      →
                                                                     ←  {"type":"ready"}
               ←  onConnectionChange(true) ←  markConnected()       ←
               →  isWebSocketConnected=true

               [后台 pingTimer 每 30s]     →  send({"type":"ping"}) →
                                           ←  {"type":"pong"}       ←

点击"开始对话"  →  state=.recording        →  startRecording()      →  {"type":"start"}
BLE 音频帧到达  →  handleBLEAudioFrame()   →  sendAudioFrame(data)  →  Binary PCM × N
点击"停止"      →  state=.processing       →  stopRecording()       →  {"type":"stop"}
                                                                     →  STT 识别 → LLM
                                                                     ←  {"type":"transcript_final"}
               ←  onEvent(.transcriptFinal)←                        ←
               →  messages.append(.user)

                                                                     ←  {"type":"llm_done"}
               ←  onEvent(.llmDone)        ←                        ←
               →  messages.append(.assistant)

                                                                     ←  {"type":"tts_start"}
               ←  onEvent(.ttsStart)       ←                        ←
               →  state=.playing; ttsPlayer.reset()

                                                                     ←  0xAA + MP3 × N
               ←  onEvent(.ttsAudio(data)) ←  parseBinaryFrame()    ←
               →  ttsPlayer.appendAudio(data)

                                                                     ←  {"type":"tts_end"}
               ←  onEvent(.ttsEnd)         ←                        ←
               →  ttsPlayer.playBuffered()
               [AVAudioPlayerDelegate.didFinish]
               →  state=.idle
```

### 7.2 BLE 音频分发路径（对现有代码的最小改动）

现有 L2CAPHandler 已有 `onAudioFrame: ((Data) -> Void)?` 回调。

在 ContentView 初始化时，根据当前 Tab 选择分发路径：
- **Tab 1 录音控制**：onAudioFrame → AudioReceiver（现有逻辑，不变）
- **Tab 3 语音聊天**：onAudioFrame → VoiceChatViewModel.handleBLEAudioFrame()

分发逻辑由 ContentView 或 AppDelegate 统一协调，通过 `activeMode: AppMode` 枚举切换：
```
enum AppMode { case recording, voiceChat }
```

---

## 8. 响应式设计

iOS 手机竖屏为主，无断点需求。

| 组件 | 适配说明 |
|------|----------|
| ChatMessageList | 占满剩余高度，键盘弹出时自动缩减（ignoresSafeArea 不设置） |
| VoiceChatActionButton | 固定底部，宽度铺满（padding 24pt 两侧） |
| ChatBubbleView | 最大宽度 75% 屏宽，长文本自动换行 |

---

## 9. 测试方案

### 9.1 测试策略

| 层级 | 范围 | 工具 | 覆盖重点 |
|------|------|------|----------|
| 单元测试 | VoiceChatViewModel 状态机 | XCTest | state 流转、messages 追加逻辑 |
| 单元测试 | PipecatWebSocketClient 消息解析 | XCTest | JSON 解析、Binary 帧识别、类型化命令序列化 |
| 单元测试 | PipecatWebSocketClient 封装约束 | XCTest | 外部不暴露协议字符串、心跳定时器行为 |
| 手动联调 | 真机 + Pipecat 本地服务 | — | 端到端全链路跑通 |

### 9.2 关键测试用例

**ViewModel 状态机**

| 用例 | 类型 | 验证点 |
|------|------|--------|
| 正常对话轮次状态流转 | 单元 | idle→recording→processing→playing→idle |
| WS 未连接时按钮禁用 | 单元 | isWebSocketConnected=false → button disabled |
| transcript_final 消息追加 | 单元 | messages 新增 role:.user 条目 |
| llm_done 消息追加 | 单元 | messages 新增 role:.assistant 条目 |
| error 事件处理 | 单元 | state=.error("STT_FAIL: ...") |

**PipecatWebSocketClient 封装层**

| 用例 | 类型 | 验证点 |
|------|------|--------|
| startRecording() 发送正确 JSON | 单元 | 序列化输出 `{"type":"start"}` |
| stopRecording() 发送正确 JSON | 单元 | 序列化输出 `{"type":"stop"}` |
| sendAudioFrame() 发送裸二进制 | 单元 | 无协议头修饰，原始 Data 透传 |
| parseBinaryFrame: 0xAA 前缀识别 | 单元 | `data[0]==0xAA` → `.ttsAudio(data.dropFirst())` |
| parseBinaryFrame: 非 0xAA 二进制 | 单元 | `data[0]==0x7B("{")` → parseJSONEvent |
| 心跳：30s 后自动发 ping | 单元 | 注入 MockTimer，触发后验证 send 调用 |
| 心跳：收到 pong 重置超时计时器 | 单元 | pong 到来后 pongTimeout 取消 |
| 断线重连：首次 1s 后重试 | 单元 | receiveNext 失败后 delay ≈ 1s 触发 connect |
| 断线重连：指数退避上限 30s | 单元 | 多次失败后 retryInterval 不超过 30s |
| ready 事件后才置 isConnected=true | 单元 | connect() 后 isConnected 仍为 false，收 ready 后为 true |

**手动联调**

| 用例 | 验证点 |
|------|--------|
| TTS 音频帧播放 | 扬声器发出 AI 回复语音 |
| 多轮对话历史展示 | 5 轮对话后界面正确显示所有气泡 |
| 服务端重启后自动重连 | 重连成功后可继续对话（不需重启 App） |
| 网络切换后自动重连 | Wi-Fi → LTE 切换后 WS 重新建立 |

---

## 10. 影响评估

### 10.1 对现有前端的影响

- `ContentView.swift`：新增 Tab 3 入口（约 5 行），新增 `AppMode` 枚举和 audio 分发逻辑
- `L2CAPHandler.swift`：**不修改**，通过 `onAudioFrame` 回调分发，现有逻辑不受影响
- `BLEManager.swift`：**不修改**
- 现有 Tab 1、Tab 2 功能：**完全不变**

### 10.2 后端接口依赖

| 接口 | 状态 | 说明 |
|------|------|------|
| WebSocket `ws://.../ws` | 待开发（pipecat-pipeline 模块） | 见后端设计文档 |
| 消息协议（JSON + Binary） | 待开发 | 见后端设计 §2.2/§2.3 |

### 10.3 代码生成指引

> 使用 `/ui-gen` 从本设计文档生成 SwiftUI 代码。
> 目标平台：iOS / SwiftUI
> 重点生成：VoiceChatView、VoiceChatViewModel、ChatBubbleView、TtsAudioPlayer 骨架
