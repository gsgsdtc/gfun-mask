# Design: 03-iOS 语音聊天 Pipecat 后端管道

> 所属模块：pipecat-pipeline
> 关联需求：docs/feat/feat-03-ios-voice-chat-pipecat.md
> 关联前端设计：docs/modules/voice-chat/design/03-ios-voice-chat-frontend-design.md
> 更新日期：2026-03-10
> 状态：草稿

## 1. 设计概述

### 1.1 目标

部署一个 Pipecat WebSocket 服务，接收 iOS 实时推送的 PCM 音频流，经由 STT → LLM → TTS 管道处理后，将文字转写和语音回复实时推回 iOS App，打通完整语音对话链路。

### 1.2 设计约束

- 音频输入格式：PCM 16kHz 16-bit mono（与 ESP32 当前输出一致，iOS 端无需转码）
- 传输协议：WebSocket（无需 Daily 账号，本地开发直连，符合开发阶段要求）
- Pipecat 版本：`pipecat-ai`（PyPI，>=0.0.40）
- 开发阶段部署：本地 Mac，iOS 与 Mac 在同一局域网，通过 IP:Port 访问
- STT/LLM/TTS 服务商可配置（通过 `.env` 文件），不硬编码
- PCM 帧大小：640B（320 samples × 2B），50fps，20ms/帧
- 不涉及持久化存储、用户认证

---

## 2. 接口设计（WebSocket 协议）

### 2.1 连接建立

```
WebSocket URL: ws://{server_ip}:{port}/ws
```

连接成功后，服务端立即发送就绪事件：
```json
{"type": "ready"}
```

### 2.2 iOS → 服务端消息

| 消息类型 | 格式 | 说明 |
|---------|------|------|
| 音频帧 | 二进制（Binary Frame） | 原始 PCM 16kHz 16-bit mono，每帧 640B |
| 开始录音 | JSON: `{"type":"start"}` | 通知服务端开始收音、初始化 STT |
| 停止录音 | JSON: `{"type":"stop"}` | 通知服务端音频结束，触发最终 STT 推断 |
| 心跳 | JSON: `{"type":"ping"}` | 保活 |

### 2.3 服务端 → iOS 消息

| 消息类型 | 格式 | 说明 |
|---------|------|------|
| 就绪 | `{"type":"ready"}` | 服务端管道初始化完成 |
| STT 中间结果 | `{"type":"transcript_partial","text":"..."}` | 实时识别（如有） |
| STT 最终结果 | `{"type":"transcript_final","text":"..."}` | 本轮用户说话内容 |
| LLM 回复（流式） | `{"type":"llm_token","text":"..."}` | 逐 token 推送 |
| LLM 回复完成 | `{"type":"llm_done","text":"完整回复"}` | LLM 完整文字 |
| TTS 开始 | `{"type":"tts_start"}` | 语音合成开始 |
| TTS 音频帧 | 二进制（Binary Frame，标识前缀见下） | PCM/MP3 TTS 音频块 |
| TTS 结束 | `{"type":"tts_end"}` | 本轮 TTS 完毕 |
| 错误 | `{"type":"error","code":"STT_FAIL","message":"..."}` | 管道异常通知 |
| 心跳响应 | `{"type":"pong"}` | 回应 ping |

> **TTS 音频帧标识**：服务端发送 TTS 音频时，在 Binary Frame 前 1 字节写入 `0xAA` 作为音频帧标志，iOS 通过首字节区分 TTS 音频帧与其他 JSON 消息。

### 2.4 错误码

| 错误码 | 说明 |
|--------|------|
| `STT_FAIL` | STT API 调用失败 |
| `LLM_FAIL` | LLM API 调用失败 |
| `TTS_FAIL` | TTS API 调用失败 |
| `AUDIO_FORMAT_ERR` | 收到的音频格式不符合预期 |
| `PIPELINE_ERR` | Pipecat 管道内部错误 |

---

## 3. 模型设计

### 3.1 服务端内部数据流

本服务为无状态无持久化设计，不涉及数据库实体。

**内存中的会话上下文**（每个 WebSocket 连接一个实例）：

| 字段 | 类型 | 说明 |
|------|------|------|
| session_id | str | UUID，连接建立时生成 |
| audio_buffer | bytes | 累积的 PCM 音频字节 |
| conversation_history | list[dict] | 当轮以内的 messages（role + content） |
| state | Enum | IDLE / RECEIVING / PROCESSING / SPEAKING |

### 3.2 会话状态机

```
IDLE ──start──▶ RECEIVING ──stop──▶ PROCESSING ──tts_done──▶ IDLE
                                         │
                                     error
                                         ▼
                                       IDLE（发 error 事件）
```

---

## 4. 逻辑设计

### 4.1 Pipecat 管道结构

```
WebSocketServerTransport (input: PCM binary)
        │
        ▼
  AudioAccumulator            ← 累积音频直到 stop 事件
        │
        ▼
  STTService                  ← Deepgram / OpenAI Whisper
  (SpeechToTextFrame)
        │
        ▼
  LLMContextAssembler         ← 组装 messages（system + history + user）
        │
        ▼
  LLMService                  ← OpenAI GPT-4o（支持流式）
  (TextFrame streaming)
        │
        ▼
  TTSService                  ← OpenAI TTS / ElevenLabs / Cartesia
  (AudioRawFrame)
        │
        ▼
WebSocketServerTransport (output: TTS binary + JSON events)
```

### 4.2 核心处理流程

**一次完整对话轮次**：

```
iOS 发送 {"type":"start"}
        │
        ▼
服务端切换 state → RECEIVING
发送 {"type":"ready"} 确认（若尚未发送）
        │
iOS 持续发送 PCM Binary Frames（640B 每帧）
        │
        ▼
AudioAccumulator 累积字节
        │
iOS 发送 {"type":"stop"}
        │
        ▼
服务端切换 state → PROCESSING
调用 STT Service（传入全量 audio_buffer）
        │
        ▼
收到 SpeechToTextFrame
  → 发送 {"type":"transcript_final","text":"..."} 给 iOS
  → 更新 conversation_history（role:user）
        │
        ▼
调用 LLM Service（stream=True）
  → 每个 token 发送 {"type":"llm_token","text":"..."}
  → 收集完整文字
  → 发送 {"type":"llm_done","text":"..."} 给 iOS
  → 更新 conversation_history（role:assistant）
        │
        ▼
发送 {"type":"tts_start"} 给 iOS
调用 TTS Service
  → 每块 PCM 音频：Binary Frame（[0xAA][audio_bytes]）
发送 {"type":"tts_end"} 给 iOS
        │
        ▼
服务端切换 state → IDLE
清空 audio_buffer
```

### 4.3 业务规则

- 每个 WebSocket 连接独立维护会话上下文，互不干扰
- 对话历史仅保留当前连接生命周期内（断连即清空）
- 历史最多保留 10 轮（20 条 messages），超出时丢弃最早的用户+助手对
- STT 输入若为空（静音帧），不触发 LLM，直接发送 `{"type":"transcript_final","text":""}`
- LLM system prompt 从环境变量 `LLM_SYSTEM_PROMPT` 读取，默认为简单助手角色

### 4.4 边界/异常处理

- **STT 超时/失败**：发送 `{"type":"error","code":"STT_FAIL"}`，state 回 IDLE，不终止连接
- **LLM 失败**：发送 `{"type":"error","code":"LLM_FAIL"}`，state 回 IDLE
- **TTS 失败**：LLM 文字已发送，TTS 发送 `{"type":"error","code":"TTS_FAIL"}`，iOS 可回退展示文字
- **客户端断连**：立即清理会话上下文，关闭管道
- **iOS 在 PROCESSING 期间发送 stop**：忽略，等当前轮完成

---

## 5. 工程结构

```
pipecat-server/
├── main.py              # 入口：FastAPI + WebSocket 路由
├── pipeline.py          # Pipecat 管道定义
├── session.py           # 会话上下文管理
├── protocol.py          # WebSocket 消息编解码（JSON + Binary）
├── config.py            # 从 .env 加载服务商配置
├── requirements.txt     # pipecat-ai, fastapi, uvicorn, python-dotenv
└── .env.example         # STT/LLM/TTS API KEY 模板
```

### 5.1 `.env.example`

```
# 服务
SERVER_HOST=0.0.0.0
SERVER_PORT=8765

# STT（选其一）
STT_PROVIDER=deepgram          # deepgram | openai_whisper
DEEPGRAM_API_KEY=xxx

# LLM
LLM_PROVIDER=openai
OPENAI_API_KEY=xxx
LLM_MODEL=gpt-4o
LLM_SYSTEM_PROMPT=你是一个有帮助的语音助手，回答简洁。

# TTS（选其一）
TTS_PROVIDER=openai            # openai | cartesia | elevenlabs
TTS_VOICE=alloy
```

---

## 6. 测试方案

### 6.1 测试策略

| 层级 | 范围 | Mock 边界 |
|------|------|----------|
| 单元测试 | `protocol.py` 编解码、`session.py` 状态机 | 无外部依赖 |
| 集成测试 | WebSocket 完整消息流 | Mock STT/LLM/TTS Service |
| 手动联调 | iOS 真机 + 本地服务端 | 真实 API Keys |

### 6.2 关键用例

| 用例 | 输入 | 期望结果 |
|------|------|---------|
| 正常对话轮次 | start → 10帧PCM → stop | 收到 transcript_final + llm_done + TTS音频 |
| 静音帧 | start → 10帧空白PCM → stop | transcript_final text="" , 不触发 LLM |
| STT 失败 | STT API 返回 500 | 收到 error(STT_FAIL)，state 回 IDLE |
| 断连后重连 | 关闭 WebSocket 后重新连接 | 新会话，历史清空，收到 ready |
| 多轮对话 | 3 轮 start/stop | 每轮独立，历史累积，LLM 上下文正确 |

---

## 7. 影响评估

### 7.1 对现有功能的影响

- Pipecat 服务为全新独立进程，与现有 iOS App 录音功能**不冲突**
- iOS 现有 `audio-player` 模块中的 WAV 录音功能保持不变

### 7.2 对其他模块的影响

- **ble-channel 模块**：无变更，iOS BLE L2CAP 读取逻辑复用
- **audio-capture 模块**：无变更，ESP32 固件不涉及

### 7.3 回滚方案

- Pipecat 服务关闭即可恢复到 Phase 2 状态
- iOS App 语音聊天 Tab 可独立禁用（feature flag via UserDefaults）
