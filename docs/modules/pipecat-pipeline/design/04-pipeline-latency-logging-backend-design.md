# Backend Design: 语音管道性能监控与对话历史存储

> 关联 feat：docs/feat/feat-04-pipeline-latency-logging.md
> 关联前端设计：docs/modules/admin/design/04-admin-frontend-design.md
> 模块：pipecat-pipeline
> 日期：2026-03-13

---

## 1. 变更范围

在 `pipecat-server/` 内增量添加以下能力，**不修改现有管道数据流**：

| 新增 | 说明 |
|------|------|
| `latency.py` | `LatencyRecord`（共享计时数据）+ `LatencyTracker`（FrameProcessor，拦截 stop 帧计时） |
| `db.py` | SQLite 初始化 + 异步 CRUD（`aiosqlite`） |
| `admin_api.py` | FastAPI Router，仅提供 `/api/admin/*` REST 数据接口；HTML 页面由独立的 `admin` 模块负责 |
| 修改 `main.py` | 注册 admin_api router；向 `DashScopeSTTService`、`TTSAudioForwarder` 注入 `LatencyRecord` |
| 修改 `dashscope_services.py` | `DashScopeSTTService.run_stt()` 记录 ASR 首包/总计时间 |
| 修改 `requirements.txt` | 新增 `aiosqlite` |

---

## 2. 数据模型

### 2.1 SQLite 表结构

```sql
CREATE TABLE IF NOT EXISTS conversations (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id   TEXT    NOT NULL,          -- WebSocket 连接 ID（uuid4）
    created_at   TEXT    NOT NULL,          -- ISO-8601，UTC
    user_text    TEXT    NOT NULL DEFAULT '',
    ai_text      TEXT    NOT NULL DEFAULT '',

    -- ASR
    asr_ttfa_ms  INTEGER,                   -- 首包时间（当前批量模式 = asr_total_ms）
    asr_total_ms INTEGER,                   -- 完整识别耗时

    -- LLM
    llm_ttft_ms  INTEGER,                   -- 首 Token 时间
    llm_total_ms INTEGER,                   -- 完整生成耗时

    -- TTS
    tts_ttfa_ms  INTEGER,                   -- 首帧音频时间
    tts_total_ms INTEGER,                   -- 完整合成耗时

    -- 整体
    e2e_ttfa_ms  INTEGER                    -- 从 stop 到首帧 TTS 音频
);

CREATE INDEX IF NOT EXISTS idx_created_at ON conversations(created_at DESC);
```

数据库文件路径：`pipecat-server/voicemask.db`（随服务启动自动创建）。

### 2.2 LatencyRecord（Python dataclass）

```
LatencyRecord
  session_id: str
  stop_time: float | None         # time.monotonic()，stop 指令到达时刻
  asr_start: float | None         # run_stt() 进入时刻
  asr_first: float | None         # 批量模式 = asr_end；流式 ASR 后为真正首包
  asr_end: float | None           # run_stt() 返回时刻
  llm_ttft: float | None          # 第一个 TextFrame 到达 TTSAudioForwarder
  llm_end: float | None           # LLMFullResponseEndFrame 到达
  tts_ttfa: float | None          # 第一个 TTSAudioRawFrame 到达 TTSAudioForwarder
  tts_end: float | None           # TTSStoppedFrame 到达
  user_text: str
  ai_text: str
  on_complete: Callable           # 异步回调，计时完整后写 DB + 输出日志
```

---

## 3. 接口设计

### 3.1 REST API（FastAPI Router，prefix `/api/admin`，文件 `admin_api.py`）

> 本模块**只提供数据接口**，不负责 HTML 页面渲染。`admin` 模块通过调用这些接口获取数据后渲染页面。

#### GET `/api/admin/stats`

概览统计，用于首页仪表盘。

**响应**：
```json
{
  "today_count": 12,
  "avg_e2e_ttfa_ms": 1050,
  "avg_asr_total_ms": 340,
  "avg_llm_ttft_ms": 490,
  "avg_tts_ttfa_ms": 220,
  "recent": [
    {
      "id": 42,
      "created_at": "2026-03-13T10:23:00Z",
      "user_text": "今天天气怎么样",
      "ai_text": "今天北京晴，气温 10-18°C...",
      "e2e_ttfa_ms": 1010
    }
  ]
}
```

`recent` 固定返回最近 5 条。

#### GET `/api/admin/conversations`

分页对话列表。

**Query 参数**：

| 参数 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `page` | int | 1 | 页码（1-based） |
| `size` | int | 20 | 每页条数，最大 100 |

**响应**：
```json
{
  "total": 156,
  "page": 1,
  "size": 20,
  "items": [
    {
      "id": 42,
      "created_at": "2026-03-13T10:23:00Z",
      "user_text": "今天天气怎么样",
      "ai_text": "今天北京晴，气温 10-18°C...",
      "e2e_ttfa_ms": 1010
    }
  ]
}
```

按 `created_at DESC` 排序。

#### GET `/api/admin/conversations/{id}`

单条对话完整数据。

**响应**：
```json
{
  "id": 42,
  "session_id": "abc-123",
  "created_at": "2026-03-13T10:23:00Z",
  "user_text": "今天天气怎么样",
  "ai_text": "今天北京晴，气温 10-18°C，适合外出。",
  "asr_ttfa_ms": 340,
  "asr_total_ms": 340,
  "llm_ttft_ms": 490,
  "llm_total_ms": 1200,
  "tts_ttfa_ms": 220,
  "tts_total_ms": 890,
  "e2e_ttfa_ms": 1050
}
```

**错误**：`404 Not Found`（id 不存在）。

---

## 4. 核心逻辑设计

### 4.1 计时埋点位置

```
WebSocket "stop" 消息
  │ iOSProtocolSerializer.deserialize → iOSStopRecordingFrame
  ▼
LatencyTracker.process_frame()
  │ record.stop_time = time.monotonic()
  ▼
DashScopeSTTService.run_stt()
  │ record.asr_start = time.monotonic()           [进入 run_stt]
  │ ... loop.run_in_executor(recognize) ...
  │ record.asr_first = time.monotonic()           [批量模式：识别返回即为首包]
  │ record.asr_end   = time.monotonic()           [yield TranscriptionFrame 前]
  ▼
TTSAudioForwarder.process_frame()
  │ TextFrame（第一个）→ record.llm_ttft = now
  │ LLMFullResponseEndFrame  → record.llm_end = now; record.ai_text = full_text
  │ TTSAudioRawFrame（第一个）→ record.tts_ttfa = now; 触发 on_complete()
  │ TTSStoppedFrame          → record.tts_end = now
  ▼
on_complete() 异步回调
  │ 1. 计算派生指标（e2e = tts_ttfa - stop_time，等）
  │ 2. await db.insert_conversation(record)
  │ 3. logger.info("[Latency] ASR=...ms | ...")
  └─ 超阈值（任一 > 1000ms）→ logger.warning("[Latency] ⚠ 慢请求 ...")
```

### 4.2 LatencyRecord 注入方式

每次 WebSocket 连接建立时（`websocket_endpoint` 函数内），创建一个新的 `LatencyRecord` 实例，通过构造函数注入到各 Processor：

```
LatencyRecord  ──注入──►  LatencyTracker (新 Processor)
               ──注入──►  DashScopeSTTService（新增 latency 参数）
               ──注入──►  TTSAudioForwarder（新增 latency 参数）
               ──注入──►  TranscriptForwarder（记录 user_text）
```

注入后各 Processor 直接赋值 `record.xxx = time.monotonic()`，无锁（单 asyncio 事件循环，无并发写入风险）。

### 4.3 数据库初始化

服务启动时（`lifespan` 或 `startup` event）执行：
```
async with aiosqlite.connect(DB_PATH) as db:
    await db.execute(CREATE TABLE SQL)
    await db.commit()
```

所有 CRUD 使用 `aiosqlite` 异步接口，不阻塞事件循环。

### 4.4 日志格式

```
[Latency] session=<8位ID> | ASR_ttfa=340ms | ASR=340ms | LLM_TTFT=490ms | LLM=1200ms | TTS_ttfa=220ms | TTS=890ms | E2E_ttfa=1050ms
```

---

## 5. 文件结构变更

```
pipecat-server/
├── main.py               # 修改：注入 LatencyRecord，注册 admin_api router，startup 初始化 DB
├── dashscope_services.py # 修改：DashScopeSTTService.run_stt() 加计时
├── latency.py            # 新增：LatencyRecord dataclass + LatencyTracker FrameProcessor
├── db.py                 # 新增：SQLite 初始化 + CRUD（voicemask.db）
├── admin_api.py          # 新增：FastAPI Router，仅 /api/admin/* 数据接口
└── requirements.txt      # 修改：新增 aiosqlite>=0.20

# HTML 页面与模板由独立的 admin 模块管理（见 admin 模块设计文档）
```

---

## 6. 影响评估

| 影响项 | 评估 |
|--------|------|
| 现有 WebSocket 功能 | **无影响**：计时逻辑纯旁路，不改变帧流向 |
| 现有 Processor 接口 | 最小侵入：`DashScopeSTTService` 新增可选 `latency` 构造参数，默认 `None` 不计时 |
| 启动时间 | 增加 < 100ms（SQLite 文件创建） |
| 请求延迟 | 增加 < 1ms（内存操作 + 异步 DB 写入在 tts_end 之后，不在关键路径） |
| 现有测试 | 不影响（新增参数有默认值，测试无需修改） |

---

## 7. 测试方案

| 类型 | 用例 |
|------|------|
| 单元测试 | `LatencyRecord` 派生指标计算正确性 |
| 单元测试 | `db.py` insert / query / pagination 正确性（in-memory SQLite） |
| 大单元测试 | 完整一次 WebSocket 会话后，DB 中存在对应记录且字段完整 |
| API 测试 | `/api/admin/stats`、`/api/admin/conversations` 响应结构正确 |
| 回归测试 | 现有 `test_pipeline.py`、`test_protocol.py`、`test_session.py` 全部通过 |
