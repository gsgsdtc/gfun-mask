# Epic-01: 口罩式实时语音助手

> **范围说明**：本 Epic 仅覆盖软件部分，包括 ESP32 固件、iOS App 和 Pipecat 云端管线。硬件设计（PCB、外壳、选型）不在讨论范围内。

---

## 一、产品愿景 (Product Vision)

打造一款**可穿戴式 AI 语音助手**——用户佩戴内置麦克风的 ESP32 口罩外设，外设固件将采集到的音频实时通过 BLE L2CAP 信道流式传输到 iOS App；iOS App 借助 Pipecat Client SDK 将音频上行到云端 Pipecat 语音管线（STT → LLM → TTS），并将合成语音实时回传播放给用户。整个交互做到"**开口即响应，无感知延迟**"。

---

## 二、用户故事 (User Stories)

| # | 角色 | 诉求 | 验收标准 |
|---|------|------|---------|
| US-01 | 用户 | 戴上口罩外设后，说一句话就能得到 AI 语音回答 | 从用户停止说话到听到响应 ≤ 1.5s（端到端） |
| US-02 | 用户 | 手机在口袋里（锁屏后台）也能正常响应 | App 后台存活 ≥ 12 小时，心跳不断 |
| US-03 | 用户 | 接电话时 AI 不会打扰通话 | 电话期间静音，挂断后自动恢复 |
| US-04 | 用户 | 播放音乐时 AI 回答不会突兀打断 | Ducking 平滑降音，AI 播完自动恢复 |

---

## 三、系统架构 (System Architecture)

```
┌──────────────────────────────────────────────────────────────┐
│                      ESP32 固件                               │
│                                                              │
│  [麦克风采集] ──► [VAD 检测] ──► [Opus 编码] ──► [L2CAP 发送] │
│                       │                                      │
│                   触发 0xFF 预警包                            │
│                   ◄─── 25s 心跳包 (0x00)                     │
└──────────────────────────────┬───────────────────────────────┘
                               │ BLE L2CAP CoC
┌──────────────────────────────▼───────────────────────────────┐
│                         iOS App                              │
│                                                              │
│  [L2CAP 接收层] ──► [帧解析] ──► [Pipecat Client SDK]        │
│        │                              │          │           │
│   预警唤醒                        音频上行    TTS 回传        │
│   AVAudioSession 热身          (WebRTC)   AVAudioEngine 播放 │
└──────────────────────────────┬───────────────────────────────┘
                               │ WebRTC / WebSocket
            ┌──────────────────▼──────────────────┐
            │          Pipecat 云端管线             │
            │                                     │
            │  [音频接收] ──► [STT] ──► [LLM]     │
            │                     ──► [TTS] ──►   │
            │                         [音频流回传] │
            └─────────────────────────────────────┘
```

### 3.1 数据流时序

```
ESP32 固件            iOS App              Pipecat Backend
    │                    │                       │
    │── VAD 检测到语音 ──►│                       │
    │── 0xFF 预警包 ─────►│                       │
    │                    │── AVAudioSession 热身  │
    │── Opus 音频帧 x N ─►│                       │
    │                    │── 音频流上行 ──────────►│
    │                    │                  STT 处理
    │                    │                  LLM 推理
    │                    │◄────────── TTS 音频流 ─│
    │                    │── AVAudioEngine 播放    │
    │── VAD 静默检测 ─────►│                       │
    │── 0xFE 结束帧 ──────►│── END_OF_UTTERANCE ──►│
    │                    │                       │
    │  (25s 心跳循环)     │                       │
    │── 0x00 心跳包 ──────►│                       │
```

---

## 四、ESP32 固件模块

### 4.1 核心职责

- **音频采集**：读取麦克风 PCM 数据，采样率 16kHz，单声道
- **VAD 检测**：基于能量门限或 WebRTC VAD 库，实时检测说话起止
- **音频编码**：Opus 编解码器压缩音频（16kHz，20ms 帧，~6kbps），降低 BLE 带宽占用
- **L2CAP 发送**：通过 BLE L2CAP CoC 信道将编码帧实时推送至 iOS
- **心跳维持**：每 25 秒发送 1 字节心跳包 `0x00`，保持 iOS App 后台存活

### 4.2 BLE 传输协议（帧定义）

| 字节标识 | 含义 | 触发条件 |
|---------|------|---------|
| `0x00` | 心跳包 | 每 25 秒定时发送 |
| `0xFF` | VAD 预警包 | 检测到说话起始，立即发送 |
| `0xFE` | 话语结束包 | 检测到说话结束 |
| `0x01` + N bytes | Opus 音频帧 | VAD 激活期间持续发送 |

### 4.3 BLE 连接参数策略

| 固件状态 | 连接间隔 | 说明 |
|---------|---------|------|
| 待机 | ~300ms | 低功耗模式，等待 VAD 触发 |
| 说话中 | ~15ms | VAD 触发后主动请求参数更新，保证音频帧吞吐 |
| 心跳期 | Light Sleep | 保留 BLE 链路状态，计时器到达时唤醒发包 |

### 4.4 固件开发要点

- 框架：**ESP-IDF**
- 动态连接参数：`esp_ble_gap_set_pref_conn_params`，VAD 触发时切换至 15ms
- L2CAP PSM：由 iOS 端注册服务后，通过 GATT 特征值广播给固件
- Opus 集成：使用 ESP-IDF 官方 Opus 组件或移植 `libopus`

---

## 五、iOS App 模块

### 5.1 蓝牙通信层

- 使用 `CoreBluetooth` 的 `CBPeripheral.openL2CAPChannel(PSM)` 建立信道
- 在 `CBL2CAPChannel.inputStream` 上循环读取数据帧
- 帧解析器按帧头分发：`0x00` 心跳 / `0xFF` 预警 / `0xFE` 结束 / `0x01+N` 音频帧

### 5.2 Pipecat Client SDK 集成

使用 **Pipecat iOS Client SDK**（Swift）与后端 Bot 建立实时会话：

```swift
// 伪代码示意
let client = PipecatClient(
    url: "wss://your-pipecat-backend/bot",
    transport: .dailyWebRTC
)
client.connect()

// 将来自 ESP32 的 Opus 帧喂给 SDK（替代本地麦克风）
func onAudioFrameReceived(_ frame: Data) {
    client.appendAudioData(frame)
}

// 接收 TTS 回传音频并播放
client.onAudioOutput = { audioData in
    audioEngine.play(audioData)
}
```

> Pipecat 支持 **Daily.co WebRTC** 或直接 **WebSocket** 作为传输层，根据后端部署方式选择。

### 5.3 后台保活策略

```
Background Modes 开启项：
  ✅ Audio, AirPlay, and Picture in Picture
  ✅ Uses Bluetooth LE accessories
```

- **25s 心跳**：L2CAP 数据到来时 iOS 自动唤醒 App，防止进程进入 Terminated 状态
- **VAD 预警唤醒**：收到 `0xFF` 后立即 `AVAudioSession.setActive(true)`，提前热身音频通路
- **CBCentralManager 状态恢复**：实现 `willRestoreState` 代理，App 被强杀后随心跳自动重启并重建 L2CAP 信道

### 5.4 音频输出策略

```swift
let session = AVAudioSession.sharedInstance()
try session.setCategory(
    .playAndRecord,
    options: [.duckOthers, .allowBluetooth]
)
try session.setActive(true)
```

- **Ducking**：播放 AI 回复时自动压低背景音乐，播完自动恢复
- **AVAudioEngine**：预装音频节点，减少首帧播放延迟
- **电话中断处理**：
  - 监听 `AVAudioSession.interruptionNotification`
  - 通话中：改用 `UIImpactFeedbackGenerator` 震动反馈替代声音
  - 通话结束：自动 `setActive(true)` 恢复发声能力

---

## 六、Pipecat 后端管线

### 6.1 管线配置

```python
pipeline = Pipeline([
    transport.input(),                        # 接收来自 iOS 的音频流
    AliyunSTTService(),                       # 阿里云实时语音识别（Paraformer 流式）
    LLMUserContextAggregator(),
    QwenLLMService(model="qwen-max"),             # 通义千问
    AliyunTTSService(),                       # 阿里云语音合成（或火山引擎）
    transport.output(),                       # 音频流回传 iOS
])
```

### 6.2 传输方案选择

| 方案 | 优点 | 缺点 | 推荐阶段 |
|------|------|------|---------|
| Daily.co WebRTC | SDK 成熟，Pipecat 官方首选 | 依赖第三方服务 | MVP |
| WebSocket 直连 | 完全自控，无第三方依赖 | 需自行处理音频协商 | 生产环境 |

### 6.3 延迟目标拆解

| 环节 | 目标延迟 |
|------|---------|
| ESP32 → iOS（BLE L2CAP） | ≤ 50ms |
| iOS → Pipecat（WebRTC） | ≤ 80ms |
| STT（阿里云 Paraformer 流式） | ≤ 300ms |
| LLM 首 token（通义千问） | ≤ 500ms |
| TTS 首帧（阿里云语音合成） | ≤ 200ms |
| **端到端总计** | **≤ 1.5s** |

---

## 七、开发阶段 (Milestones)

### Phase 1 — BLE 通道验证
- [ ] ESP32 固件跑通 L2CAP CoC Demo
- [ ] iOS App 建立 L2CAP 连接，收发原始字节
- [ ] 验证 25s 心跳、`0xFF` 预警包的收发时序
- [ ] iOS App 后台 12 小时存活压测

### Phase 2 — 音频链路打通
- [ ] 固件集成 VAD + Opus 编码，发送压缩音频帧
- [ ] iOS 解码 Opus，通过 AVAudioEngine 本地播放验证音质
- [ ] BLE 连接参数动态切换（300ms ↔ 15ms）验证

### Phase 3 — Pipecat 管线集成
- [ ] 搭建 Pipecat 后端（Docker + Daily.co WebRTC）
- [ ] iOS 集成 Pipecat Client SDK，建立 WebRTC 会话
- [ ] 将 ESP32 音频帧转接给 Pipecat SDK（替代本地麦克风输入）
- [ ] 验证 STT → LLM → TTS 完整管线端到端联调
- [ ] 测量端到端延迟，调优至 ≤ 1.5s

### Phase 4 — 后台与异常场景完善
- [ ] 电话中断：震动反馈 + 通话结束后自动恢复
- [ ] App 被强杀：`CBCentralManager` 状态恢复 + L2CAP 自动重连
- [ ] 内存警告：释放音频缓存，保留 BLE 连接
- [ ] 音频 Ducking 与背景音乐共存场景测试

---

## 八、关键风险与应对

| 风险 | 影响 | 应对策略 |
|------|------|---------|
| BLE 带宽不足导致音频卡顿 | 用户体验差 | Opus 码率降至 6kbps；连接间隔切换至 15ms |
| iOS 后台进程被强杀 | 功能中断 | `CBCentralManager` 状态恢复；25s 心跳自动重启 |
| Pipecat 管线延迟超标 | 响应感知迟钝 | 切换更近地区节点；降级至 WebSocket 直连 |
| VAD 误触发（噪音环境） | 无效请求浪费 | 调整能量门限；最短说话时长过滤（≥ 300ms） |
| App Store 后台音频审核被拒 | 无法上架 | 确保 AudioSession 始终服务于用户可感知音频 |

---

## 九、技术选型汇总

| 层次 | 技术选型 |
|------|---------|
| ESP32 固件框架 | ESP-IDF |
| 音频编码 | Opus 16kHz / 20ms 帧 / ~6kbps |
| BLE 传输 | BLE L2CAP CoC（PSM 自定义） |
| iOS BLE | CoreBluetooth L2CAP API |
| iOS 音频 | AVAudioSession + AVAudioEngine |
| iOS AI 接入 | Pipecat iOS Client SDK（Swift） |
| AI 管线框架 | Pipecat（Python） |
| 传输层 | Daily.co WebRTC（MVP）→ 自建 WebSocket（生产）|
| STT | 阿里云语音识别（Paraformer 实时流式） |
| LLM | 通义千问（qwen-max） |
| TTS | 阿里云语音合成 |
| 后端部署 | Docker + Fly.io / AWS |
