# Design: 08 - Live 聊天 Pipecat 后端（去 VAD + 打断处理）

> 所属模块：pipecat-pipeline
> 关联需求：docs/feat/feat-08-live-chat-wake-word-vad.md
> 关联固件设计：docs/modules/audio-capture/design/08-live-chat-firmware-design.md
> 关联 iOS 设计：docs/modules/voice-chat/design/08-live-chat-ios-design.md
> 更新日期：2026-04-08
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

移除 Pipecat 管道中的服务端 VAD，改由 iOS 侧的 BLE `0xFE` 帧触发音频结束；新增 `interrupt` / `wake` / `sleep` 消息处理，支持 TTS 打断和浅休眠状态记录。

### 1.2 设计约束

- Pipecat 版本：复用现有安装版本，不升级
- VAD 由 ESP32 VADNet 承担，服务端不再运行 VAD 模型
- 打断通过 Pipecat 原生 `pipeline.cancel()` / `task.cancel()` 实现
- 浅休眠仅记录 session 状态，不关闭 WebSocket 连接
- 监控埋点：新增消息类型需同步写入 `LatencyRecord` 和 SQLite

---

## 2. 接口设计

### 2.1 WebSocket 新增消息类型

**iOS → 服务端（新增）**

| 消息类型 | JSON | 说明 |
|---------|------|------|
| 打断 | `{"type":"interrupt"}` | 用户在 TTS 播放中开口说话，取消当前 LLM/TTS 任务 |
| 唤醒 | `{"type":"wake"}` | 设备从浅休眠唤醒，开始新一轮对话 |
| 休眠 | `{"type":"sleep"}` | 设备进入浅休眠，30s 无活动 |

**服务端 → iOS（无新增，复用 feat-03）**

`tts_start` / TTS 二进制帧 / `tts_end` / `transcript_final` / `llm_done` 均不变。

### 2.2 内部接口变更

| 函数/方法 | 变更类型 | 说明 |
|-----------|---------|------|
| `handle_interrupt()` | **新增** | 取消当前进行中的 LLM/TTS 任务，清空 TTS 音频队列 |
| `handle_wake()` | **新增** | 重置 session 空闲状态，记录唤醒时间戳 |
| `handle_sleep()` | **新增** | 记录 session 进入休眠时间戳 |
| VAD 相关处理逻辑 | **移除** | 删除服务端 VAD 初始化和检测代码 |

---

## 3. 模型设计

### 3.1 SessionState 变更

在现有 session 管理中新增字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `state` | `str` | `listening` / `processing` / `tts_playing` / `sleep` |
| `last_wake_at` | `float` | 最近一次唤醒时间戳（Unix） |
| `last_sleep_at` | `float` | 最近一次进入休眠时间戳（Unix） |
| `interrupt_count` | `int` | 本次会话累计打断次数（监控用） |

### 3.2 LatencyRecord 新增字段

| 字段 | 说明 |
|------|------|
| `interrupt_at` | 收到 interrupt 消息的时间戳 |
| `wake_at` | 收到 wake 消息的时间戳 |

### 3.3 SQLite conversations 表新增列

| 列名 | 类型 | 说明 |
|------|------|------|
| `interrupt_count` | INTEGER | 本轮对话打断次数 |
| `wake_count` | INTEGER | 本轮 session 唤醒次数 |

---

## 4. 逻辑设计

### 4.1 核心流程：正常对话

```
iOS                         Pipecat WebSocket Handler
 │                                    │
 │──── Binary(音频帧) ───────────────►│ 累积音频帧到缓冲区
 │                                    │
 │──── {"type":"stop"} ──────────────►│ 触发 ASR
 │                              ASR → LLM → TTS
 │◄─── {"type":"tts_start"} ─────────│
 │◄─── Binary(TTS音频) ──────────────│
 │◄─── {"type":"tts_end"} ───────────│
 │                             state = listening
```

### 4.2 核心流程：打断

```
iOS                         Pipecat WebSocket Handler
 │                                    │
 │  （TTS 正在推送中）                  │
 │──── {"type":"interrupt"} ─────────►│
 │                              handle_interrupt():
 │                              1. pipeline.cancel() / task.cancel()
 │                              2. 清空 TTS 音频发送队列
 │                              3. state = listening
 │                              4. 记录 interrupt_at，interrupt_count+1
 │                                    │
 │  （等待新的音频帧）                   │
```

### 4.3 核心流程：休眠

```
iOS                         Pipecat WebSocket Handler
 │                                    │
 │──── {"type":"sleep"} ─────────────►│ state = sleep，记录 last_sleep_at
 │──── WebSocket 断开 ───────────────►│ 连接关闭，session 释放
 │                                    │
 │  （等待唤醒）                        │
 │                                    │
 │──── WebSocket 重连 ───────────────►│ 新连接，新建 session
 │──── {"type":"wake"} ──────────────►│ state = listening，记录 last_wake_at
```

### 4.4 业务规则

- 收到 `interrupt` 时，若当前无进行中任务，记录日志但不报错
- 收到 `sleep` 消息后，iOS 随即断开连接，服务端直接释放 session
- 唤醒通过新 WebSocket 连接 + `wake` 消息触发，每次均为全新 session，LLM 上下文清空
- TTS 取消后，已发送到 iOS 的音频帧由 iOS 自行丢弃，服务端不重传

### 4.5 异常处理

| 异常 | 处理方式 |
|------|---------|
| 打断时 LLM 已完成、TTS 尚未开始 | cancel TTS 生成任务，不发送 `tts_start` |
| 打断时 TTS 已全部发送完毕 | 无需操作，记录 DEBUG 日志 |
| `wake` 后立即收到音频帧（无 `stop` 触发） | 正常累积音频，等待 `stop` |

---

## 5. 监控埋点

遵循 CLAUDE.md 监控规则：

| 指标 | 实现 |
|------|------|
| 打断响应时间 | `interrupt_at` 到下一次 `tts_start` 的时间差，写入日志 `[Latency]` |
| 打断次数 | 写入 `conversations.interrupt_count` |
| 唤醒次数 | 写入 `conversations.wake_count` |
| 慢请求告警 | 打断后超过 1000ms 未开始新一轮 ASR，输出 WARNING |

---

## 6. 测试方案

### 6.1 测试策略

| 层级 | 范围 | Mock 边界 |
|------|------|----------|
| 单元测试 | `handle_interrupt` / `handle_wake` / `handle_sleep` | Mock WebSocket、Mock Pipeline |
| 大单元测试 | 完整消息处理流程（含打断时序） | Mock ASR/LLM/TTS 服务 |
| 手动 E2E | 实机打断、浅休眠 | 真实服务 |

### 6.2 关键用例

| 用例 | 输入序列 | 期望行为 |
|------|---------|---------|
| 正常对话 | 音频帧 → stop → tts_start → tts_end | TTS 完整播放 |
| 打断（TTS 中途） | 音频帧 → stop → tts_start → interrupt | TTS 停止，interrupt_count+1 |
| 打断后新对话 | interrupt → 音频帧 → stop | 正常新一轮 ASR→LLM→TTS |
| 休眠 + 唤醒 | 断开 → 重连 → wake → 音频帧 → stop | 新 session，正常处理 |

---

## 7. 影响评估

### 7.1 对现有功能的影响

- **feat-03 `start`/`stop` 消息**：`stop` 继续使用，`start` 消息废弃（VAD 端到端，不再需要手动 start）
- **feat-04 监控**：新增 `interrupt_at` / `wake_at` 字段，需要 SQLite migration

### 7.2 回滚方案

- 移除 `interrupt` / `wake` / `sleep` handler，恢复服务端 VAD 配置
- 回滚 SQLite migration（删除新增列）
