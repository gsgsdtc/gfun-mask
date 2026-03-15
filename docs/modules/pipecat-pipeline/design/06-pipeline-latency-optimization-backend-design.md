# Design: 06-语音管道首包时间优化

> 所属模块：pipecat-pipeline
> 关联需求：docs/feat/feat-06-pipeline-latency-optimization.md
> 关联前端设计：无
> 更新日期：2026-03-15
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

通过三项并行优化将端到端首包时间（E2E_ttfa）从 **7257ms** 降至 **2000ms 以内**：
1. 切换更快的 LLM 模型，降低 TTFT（当前瓶颈 5475ms）
2. 在 LLM 和 TTS 之间引入 `SentenceAggregator`，实现句子级流水线（LLM 边输出边合成）
3. 修复 `TTSAudioForwarder` 在多句子场景下的 `on_complete` 触发逻辑

### 1.2 设计约束

- 不修改 `LatencyRecord` 结构和监控埋点逻辑（feat-04 约束）
- 不引入新的第三方依赖（DashScope SDK 已有，无需新增）
- 回复质量不可明显下降（模型选型需验证）
- `e2e_ttfa_ms`、`llm_ttft_ms`、`tts_ttfa_ms` 等指标计算语义保持不变

---

## 2. 接口设计

### 2.1 新增/变更 API

无新增外部 API。

### 2.2 新增/变更内部接口

| 函数/方法 | 说明 | 变更类型 |
|-----------|------|---------|
| `SentenceAggregator.process_frame(frame, direction)` | 累积 LLM Token，按句子边界切分后下发 | 新增 |
| `TTSAudioForwarder.process_frame(frame, direction)` | 支持多句 TTS 场景下的生命周期管理 | 修改 |
| `build_pipeline(...)` | 管道中插入 `SentenceAggregator` | 修改 |
| `Config.LLM_MODEL` | 默认值改为更快的模型 | 修改（配置） |

### 2.3 iOS 协议帧变更

无变更。`tts_start` / `tts_end` 仍各发一次（整轮对话级别），多句子流水线对 iOS 端透明。

---

## 3. 模型设计

### 3.1 无数据库变更

本次优化不新增数据库字段，`conversations` 表结构保持不变。

### 3.2 LatencyRecord 语义澄清

多句子流水线下各时间戳语义不变：

| 字段 | 语义（不变） | 多句子场景说明 |
|------|-------------|---------------|
| `llm_ttft` | LLM 第一个 `TextFrame` 到达时刻 | 第一个 token，不变 |
| `tts_ttfa` | TTS 第一帧音频到达时刻 | **第一句话**的第一帧，只记录一次 |
| `tts_end` | 最后一个 `TTSStoppedFrame` 到达时刻 | 每次 `TTSStopped` 都更新，最终值为最后一句 |
| `e2e_ttfa_ms` | `stop_time → tts_ttfa` | 流水线后 = ASR + LLM_first_sentence + TTS_sentence1，显著缩短 |

---

## 4. 逻辑设计

### 4.1 优化一：LLM 模型切换

**变更范围**：`config.py` 中 `LLM_MODEL` 默认值。

**候选模型**（均通过 DashScope OpenAI 兼容接入点可用）：

| 模型 | 特点 | 建议场景 |
|------|------|---------|
| `qwen-turbo` | 当前，TTFT≈5475ms | 基线 |
| `qwen-turbo-latest` | 官方最新 turbo 版本，通常优于固定版 | 首选尝试 |
| `qwen2.5-0.5b-instruct` | 极小参数量，TTFT 最低，质量有损 | 备选 |

**验证方式**：逐一修改 `LLM_MODEL` 后进行 5 次语音交互，对比 Admin 后台 `LLM_TTFT` 均值。

---

### 4.2 优化二：SentenceAggregator（句子级流水线核心）

**当前问题**：

```
LLM 输出 (串行)
  TextFrame(token1) ─┐
  TextFrame(token2)  │  → TTSService 内部积累全部 tokens
  ...                │    → LLMFullResponseEndFrame 到达后
  LLMFullResponseEnd ┘    → run_tts(full_text) 调用一次
                                ↓
                         DashScope TTS 合成全文 (~830ms)
                                ↓
                         第一帧音频到客户端   ← E2E = 7257ms
```

**优化后（句子级流水线）**：

```
LLM 流式输出 (流水线)
  TextFrame(tokens) → SentenceAggregator ─┬─ TextFrame("你好！")
                                           │   → run_tts("你好！") (~400ms) → 第一帧音频 ← E2E ↓↓
                                           ├─ TextFrame("我是语音助手。")
                                           │   → run_tts("我是...") (~400ms, 并行于 LLM 继续输出)
                                           └─ LLMFullResponseEndFrame (flush 剩余 buffer 后转发)
```

**`SentenceAggregator` 详细逻辑**：

```
状态：
  buffer: list[str] = []

process_frame(TextFrame):
  buffer.append(frame.text)
  joined = "".join(buffer)
  如果 joined 以句子边界结尾 (。！？…\n!?):
    emit TextFrame(joined.strip())
    buffer = []
  否则:
    pass（继续积累）

process_frame(LLMFullResponseEndFrame):
  如果 buffer 非空:
    emit TextFrame("".join(buffer).strip())   # flush 剩余片段
    buffer = []
  emit LLMFullResponseEndFrame               # 透传，供 TTSAudioForwarder 感知 LLM 完成

process_frame(其他帧):
  透传
```

**句子边界字符集**：`。！？…\n!?`（中文和英文标点各一组）

**边界情况**：
- LLM 回复无标点（如纯数字）→ `LLMFullResponseEndFrame` 时 flush，作为一句整体合成
- 极短回复（≤2字）→ 同上，不拆分

---

### 4.3 优化三：TTSAudioForwarder 多句子生命周期修复

**当前问题**：`on_complete` 在每个 `TTSStoppedFrame` 时触发，流水线化后会过早触发（第一句话结束就调用了 on_complete）。

**修改后的状态机**：

```
字段：
  _tts_active: int = 0          # 当前进行中的 TTS 句子数
  _llm_done: bool = False        # LLM 是否已全部输出完毕
  _first_audio_recorded: bool = False  # 本轮是否已记录 tts_ttfa（只记一次）
  _tts_started_sent: bool = False      # iOS tts_start 是否已发送（只发一次）

TTSStartedFrame:
  _tts_active += 1
  如果 not _tts_started_sent:
    → iOS 发送 {"type": "tts_start"}
    _tts_started_sent = True

TTSAudioRawFrame:
  如果 not _first_audio_recorded:
    record.tts_ttfa = now()
    _first_audio_recorded = True
  → iOS 发送音频数据

TTSStoppedFrame:
  record.tts_end = now()          # 每次更新，最终值为最后一句
  _tts_active -= 1
  如果 _llm_done and _tts_active == 0:
    → iOS 发送 {"type": "tts_end"}
    await record.on_complete(record)
    _reset()                       # 重置本轮所有状态

LLMFullResponseEndFrame:
  _llm_done = True
  如果 _tts_active == 0:           # 空回复边界情况
    → iOS 发送 {"type": "tts_end"}（如果 tts_start 已发）
    await record.on_complete(record)
    _reset()
  透传帧

_reset():
  _tts_active = 0
  _llm_done = False
  _first_audio_recorded = False
  _tts_started_sent = False
```

---

### 4.4 管道组装变更（build_pipeline）

**当前**：
```python
Pipeline([
    ...
    llm,
    LLMTextCapture(record=record),     # 记录 TTFT，捕获全文
    tts,                               # 等待 LLMFullResponseEndFrame 后批量合成
    TTSAudioForwarder(record=record),
    ...
])
```

**修改后**：
```python
Pipeline([
    ...
    llm,
    LLMTextCapture(record=record),
    SentenceAggregator(),              # 新增：句子级切分，LLM→TTS 流水线
    tts,                               # 现在每句话独立调用 run_tts()
    TTSAudioForwarder(record=record),  # 修改：支持多句 TTS 生命周期
    ...
])
```

---

### 4.5 预期延迟改善分析

**理论推算**（基于实测基线）：

| 优化项 | 改善目标 | 说明 |
|--------|---------|------|
| 模型切换 | LLM TTFT 从 5475ms → 预期 ≤1000ms | 取决于模型实测 |
| 流水线 | TTS 从"等 LLM 全完成"→"等 LLM 第一句" | 节省 = LLM 剩余生成时间 |
| 两者叠加 | E2E_ttfa ≈ ASR(951) + LLM_first_sentence + TTS_first(830) | 目标 ≤2000ms |

**流水线增益公式**：
节省 = `LLM_total_ms - LLM_first_sentence_ms`
当前 `LLM_total=5591ms`，若第一句约 1000ms，节省约 4591ms（理论上限）

---

### 4.6 边界与异常处理

| 场景 | 处理方式 |
|------|---------|
| LLM 回复为空 | `LLMTextCapture` 不发 `TextFrame`，`SentenceAggregator` flush 为空，直接透传 `LLMFullResponseEndFrame`；`TTSAudioForwarder` 收到 `LLMFullResponseEndFrame` 且 `_tts_active==0`，触发 `on_complete` |
| TTS 合成失败（某句） | `DashScopeTTSService` 发出 `ErrorFrame`，`TTSStoppedFrame` 不会发出；`_tts_active` 不减，`on_complete` 不触发 → 需后续加超时保护（非本 feat 范围） |
| LLM 只输出无标点短回复 | `SentenceAggregator` 在 `LLMFullResponseEndFrame` 时 flush，整体作为一句，退化为单句串行，行为与当前一致 |
| `SentenceAggregator` buffer 中途清空 | 不会。只在边界字符或 `LLMFullResponseEndFrame` 时 emit |

---

## 5. 测试方案

### 5.1 测试策略

| 层级 | 范围 | Mock 边界 |
|------|------|----------|
| 单元测试 | `SentenceAggregator.process_frame` 各分支 | 无外部依赖，纯 frame 逻辑 |
| 单元测试 | `TTSAudioForwarder` 多句状态机 | Mock `LatencyRecord.on_complete` |
| 大单元测试 | 完整 Pipeline（STT→LLM→TTS） | Mock DashScope API 网络调用 |
| 手动验收 | 实际语音交互 × 5 次 | 无 Mock，测量真实延迟 |

### 5.2 关键用例

| 用例 | 输入 / 条件 | 期望结果 |
|------|------------|---------|
| 单句回复 | LLM 输出 "好的。" | `SentenceAggregator` 一次 emit，TTS 一次合成，`on_complete` 正常触发一次 |
| 多句回复 | LLM 输出 "你好！我是助手。有什么可以帮你？" | `SentenceAggregator` 分 3 次 emit，`on_complete` 在第 3 句 TTSStopped 后触发一次 |
| 无标点回复 | LLM 输出 "1加1等于2" | `LLMFullResponseEndFrame` 时 flush，整体一句合成，正常完成 |
| 空回复 | LLM 输出空字符串 | 无 TTS 调用，`on_complete` 在 `LLMFullResponseEndFrame` 时触发 |
| tts_ttfa 仅记一次 | 多句回复 | `record.tts_ttfa` 只记第一句第一帧，后续句子不覆盖 |
| tts_end 为最后一句 | 多句回复 | `record.tts_end` 等于最后一个 `TTSStopped` 时间戳 |
| iOS 协议不变 | 任意回复 | `tts_start` 发送一次，`tts_end` 发送一次，与当前行为一致 |

---

## 6. 影响评估

### 6.1 对现有功能的影响

| 功能 | 影响 | 说明 |
|------|------|------|
| 延迟监控（feat-04） | 无 | `LatencyRecord` 结构和计算逻辑不变 |
| Admin 后台数据写入 | 无 | `on_complete` 触发语义不变（每轮对话一次） |
| iOS 协议 | 无 | `tts_start`/`tts_end` 仍各发一次 |
| 对话历史管理 | 无 | `context_aggregator` 不受影响 |
| 错误处理路径 | 低风险 | `ErrorFrame` 路径未修改 |

### 6.2 对其他模块的影响

- `core/latency.py`：不修改
- `services/dashscope.py`：不修改（`DashScopeTTSService.run_tts()` 逻辑不变，只是被多次调用）
- `api/admin.py`：不修改

### 6.3 回滚方案

1. **模型切换回滚**：修改 `.env` 中 `LLM_MODEL=qwen-turbo`，重启服务，无代码变更
2. **流水线回滚**：从 `build_pipeline` 中移除 `SentenceAggregator()`，恢复 `TTSAudioForwarder` 旧逻辑，重启服务

两项变更均相互独立，可单独回滚。
