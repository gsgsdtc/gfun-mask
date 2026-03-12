# Module Spec: pipecat-pipeline

> 模块：Pipecat WebSocket 语音处理管道
> 最近同步：2026-03-12
> 状态：Phase 3 完成（iOS 语音聊天全链路验证通过）

---

## 1. 模块概述

基于 [Pipecat](https://github.com/pipecat-ai/pipecat) 框架的云端语音处理服务，通过 WebSocket 接收 iOS 客户端的 PCM 音频，经 STT → LLM → TTS 管道处理后，将合成音频和文字回传 iOS。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | iOS 客户端（VoiceMaskApp），通过 WebSocket 发送录音控制指令和 PCM 音频 |
| 下游 | 无（终点节点） |
| 输入 | WebSocket 控制 JSON（start/stop/ping）、WebSocket 二进制 PCM（16kHz，16-bit，单声道） |
| 输出 | WebSocket 控制 JSON（ready/transcript_final/llm_done/tts_start/tts_end/pong）、WebSocket 二进制 MP3（以 `0xAA` 前缀标识） |

### 1.2 技术选型

| 组件 | 技术 | 说明 |
|------|------|------|
| Web 框架 | FastAPI + Uvicorn | 承载 WebSocket endpoint |
| 管道框架 | pipecat-ai >= 0.0.100 | 语音处理流水线编排 |
| STT | DashScope Paraformer (`paraformer-realtime-v2`) | 阿里云 ASR，批量识别 WAV 文件 |
| LLM | DashScope Qwen（OpenAI 兼容接口，`qwen-turbo`） | 通义千问，生成回复 |
| TTS | DashScope CosyVoice (`cosyvoice-v1`) | 阿里云 TTS，合成 MP3 |

---

## 2. 协议接口

### 2.1 WebSocket 端点

| 属性 | 值 |
|------|----|
| 路径 | `/ws` |
| 默认地址 | `ws://0.0.0.0:8765/ws` |
| 协议 | 标准 WebSocket（RFC 6455） |
| 消息类型 | 文本（JSON）+ 二进制（PCM / MP3） |

### 2.2 iOS → 服务端（上行）

#### 2.2.1 文本消息（JSON）

| 消息类型 | 结构 | 说明 |
|---------|------|------|
| `start` | `{"type":"start"}` | 开始一次录音会话，服务端清空缓冲区 |
| `stop`  | `{"type":"stop"}` | 结束录音，触发 STT 识别 |
| `ping`  | `{"type":"ping"}` | 心跳探测，服务端回 `pong`；由客户端每 30s 自动发送 |

#### 2.2.2 二进制消息（音频帧）

| 格式 | 说明 |
|------|------|
| 裸 PCM 字节 | 16kHz，16-bit，单声道，无 WAV 头 |
| 帧大小 | 由客户端决定（通常为 BLE MTU 大小，约 512 字节） |
| 传输区间 | `start` 之后、`stop` 之前 |

### 2.3 服务端 → iOS（下行）

#### 2.3.1 文本消息（JSON）

| 消息类型 | 结构 | 触发时机 |
|---------|------|---------|
| `ready` | `{"type":"ready"}` | WebSocket 连接建立后立即发送 |
| `transcript_final` | `{"type":"transcript_final","text":"<识别文本>"}` | STT 识别完成 |
| `llm_done` | `{"type":"llm_done","text":"<LLM 回复>"}` | LLM 生成完整回复后 |
| `tts_start` | `{"type":"tts_start"}` | TTS 合成开始，音频帧即将到来 |
| `tts_end` | `{"type":"tts_end"}` | TTS 所有音频帧发送完毕 |
| `pong` | `{"type":"pong"}` | 响应客户端 `ping` |

#### 2.3.2 二进制消息（TTS 音频帧）

| 格式 | 说明 |
|------|------|
| `0xAA` + MP3 数据 | 首字节固定为 `0xAA`，其余为 MP3 片段 |
| 音频格式 | MP3，22050Hz，单声道，256kbps |
| 分块大小 | 4096 字节/帧 |
| 传输区间 | `tts_start` 之后、`tts_end` 之前 |

### 2.4 交互时序

```
iOS                                     Pipecat Server
 │                                            │
 │──── WS Connect ────────────────────────►  │
 │  ◄─── {"type":"ready"} ────────────────   │  连接建立即发
 │                                            │
 │──── {"type":"start"} ───────────────────► │  开始录音
 │──── [PCM binary] × N ───────────────────► │  持续发送音频帧
 │──── {"type":"stop"} ────────────────────► │  停止录音，触发 STT
 │                                            │
 │  ◄─── {"type":"transcript_final",...} ─   │  STT 结果
 │  ◄─── {"type":"llm_done",...} ──────────  │  LLM 完整回复
 │  ◄─── {"type":"tts_start"} ─────────────  │  TTS 开始
 │  ◄─── [0xAA + MP3] × N ─────────────────  │  TTS 音频块
 │  ◄─── {"type":"tts_end"} ───────────────  │  TTS 结束
 │                                            │
 │──── {"type":"ping"} ────────────────────► │  心跳（可随时发）
 │  ◄─── {"type":"pong"} ──────────────────  │
```

---

## 3. 模块结构

```
pipecat-server/
├── main.py                  # FastAPI 应用、WebSocket endpoint、Pipeline 编排
│   ├── iOSStartRecordingFrame   # 控制帧：开始录音
│   ├── iOSStopRecordingFrame    # 控制帧：停止录音
│   ├── iOSPingFrame             # 控制帧：心跳
│   ├── iOSAudioFrame            # 音频帧（绕过 InputAudioRawFrame 异步队列）
│   ├── iOSProtocolSerializer    # WebSocket ↔ Pipecat 帧序列化/反序列化
│   └── iOSAudioAccumulator      # 音频累积器（start~stop 区间内收集 PCM）
├── dashscope_services.py    # DashScope STT / TTS Pipecat 服务封装
│   ├── DashScopeSTTService      # Paraformer STT（PCM → WAV → 识别 → TranscriptionFrame）
│   └── DashScopeTTSService      # CosyVoice TTS（文本 → MP3 → TTSAudioRawFrame）
└── config.py                # 环境变量读取（API Key、模型、服务器地址）
```

---

## 4. 接口定义

### 4.1 Pipecat 管道构成

```
WebSocket Input
    │  反序列化（iOSProtocolSerializer.deserialize）
    ▼
iOSAudioAccumulator
    │  start → UserStartedSpeakingFrame
    │  audio → 累积 PCM
    │  stop  → InputAudioRawFrame + UserStoppedSpeakingFrame
    │  ping  → OutputTransportMessageUrgentFrame(pong) [直接下行]
    ▼
DashScopeSTTService
    │  InputAudioRawFrame → 识别 → TranscriptionFrame
    ▼
LLMContextAggregator (user)
    │  TranscriptionFrame → 加入对话历史
    ▼
QwenLLMService
    │  LLMContext → 调用 Qwen API → TextFrame / LLMFullResponseEndFrame
    ▼
DashScopeTTSService
    │  TextFrame → 合成 → TTSStartedFrame + TTSAudioRawFrame×N + TTSStoppedFrame
    ▼
WebSocket Output
    │  序列化（iOSProtocolSerializer.serialize）
    ▼
LLMContextAggregator (assistant)
    │  AI 回复加入对话历史
```

### 4.2 关键帧映射

| Pipecat 帧 | 序列化输出 | 方向 |
|-----------|-----------|------|
| `TranscriptionFrame` | `{"type":"transcript_final","text":"..."}` | 下行（JSON） |
| `TextFrame` | `{"type":"llm_done","text":"..."}` | 下行（JSON） |
| `TTSStartedFrame` | `{"type":"tts_start"}` | 下行（JSON） |
| `TTSAudioRawFrame` | `bytes([0xAA]) + frame.audio` | 下行（二进制） |
| `TTSStoppedFrame` | `{"type":"tts_end"}` | 下行（JSON） |
| `OutputTransportMessageUrgentFrame` | `frame.message`（直接透传） | 下行（紧急） |
| 二进制 WebSocket 消息 | → `iOSAudioFrame` | 上行 |
| `{"type":"start"}` | → `iOSStartRecordingFrame` | 上行 |
| `{"type":"stop"}` | → `iOSStopRecordingFrame` | 上行 |
| `{"type":"ping"}` | → `iOSPingFrame` | 上行 |

### 4.3 设计约束

| 约束 | 说明 |
|------|------|
| `enable_rtvi=False` | 禁用 Pipecat 内置 RTVI 握手，否则无 RTVI 握手的客户端会被拒绝（触发 403） |
| `allow_interruptions=False` | 禁用打断；TTS 播放期间 iOS 新录音不会中断当前管道 |
| `audio_passthrough=False` | STT 服务不将音频帧透传下游，避免 `AudioRawFrame` 流入输出管道引发异常 |
| `iOSAudioFrame` 绕过异步队列 | `InputAudioRawFrame` 会被输入 transport 路由至 `_audio_in_queue`（异步），导致 `stop` 帧先于音频帧到达累积器；`iOSAudioFrame(Frame)` 直接走同步管道，保证时序 |
| `await websocket.accept()` 必须前置 | FastAPI WebSocket 未 accept 直接关闭会产生 403；须在创建 transport 之前 accept |
| `ready` 直接发送 | `StartFrame` 经过 transport 的 `start()` 而非 `_write_frame()`，序列化器不会被调用；`ready` 须在 `on_client_connected` 中通过 `client.send_text()` 直接发送 |

---

## 5. 状态机

### 服务端每次 WebSocket 会话状态

```
[IDLE]
  │ WS Connect
  ▼
[READY]         ── "ready" 消息已发，等待录音指令
  │ {"type":"start"}
  ▼
[RECORDING]     ── 累积 PCM 帧
  │ {"type":"stop"}
  ▼
[PROCESSING]    ── STT → LLM → TTS
  │ TTSStoppedFrame
  ▼
[READY]         ── 回到等待状态
  │ WS Disconnect
  ▼
[CLOSED]
```

---

## 6. 核心逻辑

### 6.1 音频累积（iOSAudioAccumulator）

```
on iOSStartRecordingFrame:
    _audio_buffer.clear()
    _recording = True
    push(UserStartedSpeakingFrame)

on iOSAudioFrame (recording=True):
    _audio_buffer.append(frame.audio)

on iOSStopRecordingFrame:
    _recording = False
    if _audio_buffer:
        combined = join(_audio_buffer)
        push(InputAudioRawFrame(audio=combined, sample_rate=16000))
    _audio_buffer.clear()
    push(UserStoppedSpeakingFrame)

on iOSPingFrame:
    push(OutputTransportMessageUrgentFrame(message='{"type":"pong"}'))
```

### 6.2 STT 处理（DashScopeSTTService）

1. 将原始 PCM 封装为 WAV（写入临时文件）
2. 在线程池中调用 `dashscope.audio.asr.Recognition.call(wav_path)`
3. 提取 `output.sentence[].text` 拼接为完整识别结果
4. yield `TranscriptionFrame(text=result)`

### 6.3 TTS 处理（DashScopeTTSService）

1. 在线程池中调用 `dashscope.audio.tts_v2.SpeechSynthesizer.call(text)`
2. 获取 MP3 字节
3. yield `TTSStartedFrame`
4. 按 4096 字节分块 yield `TTSAudioRawFrame`
5. yield `TTSStoppedFrame`

---

## 7. 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `DASHSCOPE_API_KEY` | 环境变量 | 阿里云 API Key（必填） |
| `SERVER_HOST` | `0.0.0.0` | 监听地址 |
| `SERVER_PORT` | `8765` | 监听端口 |
| `STT_MODEL` | `paraformer-realtime-v2` | 语音识别模型 |
| `LLM_MODEL` | `qwen-turbo` | 大语言模型 |
| `TTS_MODEL` | `cosyvoice-v1` | 语音合成模型 |
| `TTS_VOICE` | `longxiaochun` | TTS 音色 |
| `MAX_HISTORY_TURNS` | `10` | LLM 对话历史最大轮数 |
| `LLM_SYSTEM_PROMPT` | （见 config.py） | LLM 系统提示词 |

---

## 8. 验收状态

| 验收项 | 状态 |
|--------|------|
| WebSocket 连接建立后收到 `ready` | ✅ |
| `ping` → `pong` 心跳正常 | ✅ |
| `start` + PCM 帧 + `stop` → STT 识别结果非空 | ✅ |
| STT 结果 → LLM → `transcript_final` + `llm_done` 事件 | ✅ |
| LLM 回复 → TTS → `tts_start` + MP3 帧 + `tts_end` 事件 | ✅ |
| iOS 客户端 MP3 播放正常 | ✅ |
| 多轮对话历史保持 | ✅ |

---

## 9. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-11 | feat #03 | 初始实现：FastAPI WebSocket + Pipecat 管道，DashScope STT/LLM/TTS 全链路 |
| 2026-03-11 | fix | 修复 STTSettings/TTSSettings NOT_GIVEN 校验失败导致 403 |
| 2026-03-11 | fix | 修复 WebSocket 未 accept 导致 403 |
| 2026-03-11 | fix | 修复 RTVIProcessor 默认启用导致客户端即断 |
| 2026-03-11 | fix | 修复 StartFrame 不经序列化器，改为直接发送 ready 消息 |
| 2026-03-11 | fix | 修复 pong 帧方向错误，改用 OutputTransportMessageUrgentFrame |
| 2026-03-11 | fix | 修复 InputAudioRawFrame 异步队列时序问题，引入 iOSAudioFrame 绕过队列 |
| 2026-03-12 | feat #03 | Phase 3 完成，iOS 全链路验证通过（STT/LLM/TTS + iOS 气泡展示） |
