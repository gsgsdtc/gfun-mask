# Backend Design: pipecat-server 目录结构重构

> @doc     docs/feat/feat-05-pipecat-server-refactor.md
> @date    2026-03-14
> @scope   仅后端（pipecat-server 内部重构，无对外接口变更）

---

## 1. 现状分析

### 1.1 现有文件清单

| 文件 | 行数 | 当前角色 |
|------|------|---------|
| `main.py` | 404 | FastAPI 应用 + WebSocket endpoint + Pipecat 管道编排 + 所有 FrameProcessor 定义 |
| `config.py` | 38 | 环境变量读取（不变） |
| `dashscope_services.py` | 191 | Pipecat 专用 DashScope STT/TTS 封装 |
| `services.py` | 163 | 独立 async 函数封装（stt/llm/tts），不依赖 Pipecat |
| `pipeline.py` | 94 | VoicePipeline（非 Pipecat 轻量抽象，依赖注入 stt/llm/tts fn） |
| `protocol.py` | 78 | WebSocket 消息编解码（JSON 控制 + 二进制音频） |
| `session.py` | 80 | 会话状态机 + 对话历史管理 |
| `admin_api.py` | 49 | Admin REST API（FastAPI Router） |
| `latency.py` | 137 | LatencyRecord 数据类 + LatencyTracker 处理器 |
| `db.py` | 160 | SQLite 异步 CRUD |
| `test_*.py` × 7 | ~1100 | 单元/集成测试，与业务代码平铺 |

**关键发现**：存在两套并行实现：
- **Pipecat 生产链路**：`main.py` + `dashscope_services.py`（当前运行时）
- **独立抽象层**：`pipeline.py` + `protocol.py` + `session.py` + `services.py`（有测试覆盖，但未被 main.py 使用）

### 1.2 main.py 职责过重

```
main.py（404行）内容分布：
├── 日志配置            (~15行)
├── 自定义 Frame 定义    (~5行)   ← 应归 pipeline 层
├── iOSProtocolSerializer (~35行) ← 应归 pipeline 层
├── PingHandler          (~10行)  ← 应归 pipeline 层
├── TranscriptForwarder  (~20行)  ← 应归 pipeline 层
├── LLMTextCapture       (~25行)  ← 应归 pipeline 层
├── TTSAudioForwarder    (~40行)  ← 应归 pipeline 层
├── FastAPI 应用         (~15行)  ← 保留
├── lifespan             (~10行)  ← 保留
├── WebSocket endpoint   (~80行)  ← 提取 build_pipeline 后精简至 ~25行
└── uvicorn 入口         (~8行)   ← 保留
```

---

## 2. 目标目录结构

```
pipecat-server/
├── main.py              # 应用入口：app 创建 + 中间件 + 路由挂载 + lifespan + uvicorn
├── config.py            # 环境配置（不变）
│
├── api/                 # HTTP API 层
│   ├── __init__.py
│   └── admin.py         # ← admin_api.py
│
├── pipeline/            # Pipecat 管道层
│   ├── __init__.py
│   ├── processors.py    # FrameProcessor 子类（从 main.py 提取）
│   ├── builder.py       # build_pipeline() + iOSProtocolSerializer + iOSPingFrame
│   └── voice.py         # VoicePipeline 独立抽象（← pipeline.py）
│
├── services/            # 外部服务封装
│   ├── __init__.py
│   ├── dashscope.py     # Pipecat 专用封装（← dashscope_services.py）
│   └── functions.py     # 独立 async 函数（← services.py）
│
├── core/                # 核心基础设施（共享）
│   ├── __init__.py
│   ├── latency.py       # ← latency.py
│   ├── db.py            # ← db.py
│   ├── protocol.py      # WebSocket 编解码（← protocol.py）
│   └── session.py       # 会话状态机（← session.py）
│
└── tests/               # 所有测试文件
    ├── conftest.py      # sys.path 设置，使包导入正常工作
    ├── test_latency.py
    ├── test_db.py
    ├── test_admin_api.py
    ├── test_e2e.py
    ├── test_pipeline.py
    ├── test_protocol.py
    └── test_session.py
```

---

## 3. 各层职责边界

| 层 | 目录 | 职责 | 允许依赖 |
|----|------|------|---------|
| 应用入口 | `main.py` | FastAPI 创建、中间件注册、路由挂载、WebSocket endpoint 骨架、uvicorn | 所有层 |
| API 层 | `api/` | HTTP 路由处理、请求/响应序列化 | `core/` |
| 管道层 | `pipeline/` | Pipecat FrameProcessor、管道组装、协议序列化 | `core/`, `services/` |
| 服务层 | `services/` | 封装外部 AI API（STT/LLM/TTS） | `config.py` |
| 核心层 | `core/` | 与框架无关的基础设施（DB、延迟、协议、会话） | `config.py` |
| 测试层 | `tests/` | 测试用例 | 所有层（测试时导入） |

**禁止的依赖方向**：`core/` 不得依赖 `pipeline/` 或 `api/`；`services/` 不得依赖 `pipeline/` 或 `api/`。

---

## 4. 文件迁移映射

### 4.1 直接移动（内容不变，仅路径变化）

| 原路径 | 新路径 | 说明 |
|--------|--------|------|
| `admin_api.py` | `api/admin.py` | 函数名不变，router 变量名保持 `router` |
| `latency.py` | `core/latency.py` | 内容不变 |
| `db.py` | `core/db.py` | 内容不变 |
| `protocol.py` | `core/protocol.py` | 内容不变 |
| `session.py` | `core/session.py` | 内容不变 |
| `dashscope_services.py` | `services/dashscope.py` | 内容不变 |
| `services.py` | `services/functions.py` | 内容不变（旧文件名 `services` 过于泛化） |
| `pipeline.py` | `pipeline/voice.py` | 内容不变（VoicePipeline 抽象） |
| `test_*.py` × 7 | `tests/test_*.py` | 内容不变（imports 因包路径改变需更新） |

### 4.2 从 main.py 提取

**提取到 `pipeline/processors.py`**（以下 class 从 main.py 剪切）：

| Class | 说明 |
|-------|------|
| `PingHandler` | 心跳处理 |
| `TranscriptForwarder` | STT 结果转发 |
| `LLMTextCapture` | LLM 文本拦截（必须在 TTS 之前） |
| `TTSAudioForwarder` | TTS 音频转发 + on_complete 触发 |

**提取到 `pipeline/builder.py`**（以下从 main.py 剪切）：

| 内容 | 说明 |
|------|------|
| `iOSPingFrame` | 自定义控制帧 |
| `iOSProtocolSerializer` | WebSocket ↔ Pipecat 帧转换 |
| `TTS_AUDIO_PREFIX` 常量 | 0xAA 前缀 |
| `build_pipeline(websocket, record, on_complete)` | 新增工厂函数，封装 transport/stt/llm/tts/processors/pipeline 创建逻辑 |

**保留在 main.py 的内容**：
- 日志配置（loguru setup）
- `lifespan` + FastAPI app 创建
- CORS 中间件
- `app.include_router(admin_api.router)`
- WebSocket endpoint（骨架：`async def websocket_endpoint`，调用 `build_pipeline`）
- `uvicorn.run`

### 4.3 import 路径变更汇总

| 变更前 | 变更后 |
|--------|--------|
| `from latency import LatencyRecord, LatencyTracker` | `from core.latency import LatencyRecord, LatencyTracker` |
| `import db as db_module` | `from core import db as db_module` |
| `import admin_api` | `from api import admin as admin_api` |
| `from dashscope_services import DashScopeSTTService, DashScopeTTSService` | `from services.dashscope import DashScopeSTTService, DashScopeTTSService` |
| `from config import Config` | `from config import Config`（不变，config.py 留在根目录） |
| 测试中 `from latency import ...` | `from core.latency import ...` |
| 测试中 `from admin_api import router` | `from api.admin import router` |
| 测试中 `from pipeline import VoicePipeline` | `from pipeline.voice import VoicePipeline` |
| 测试中 `from protocol import decode_message` | `from core.protocol import decode_message` |
| 测试中 `from session import Session` | `from core.session import Session` |

---

## 5. tests/conftest.py 设计

测试文件移动到 `tests/` 子目录后，`pipecat-server/` 根目录不再是 Python path 默认位置，需要显式添加。

```
# tests/conftest.py 职责：
# - 将 pipecat-server/ 根目录添加到 sys.path
# - 确保 `from core.latency import ...` 等绝对导入正常工作
# - 不写任何业务 fixture（fixture 按需在各 test_*.py 中定义）
```

---

## 6. main.py 目标结构（< 80 行）

```
main.py 拆解后的内容：
├── imports（~15行）
├── 日志配置（~12行）
├── lifespan（~10行）
├── FastAPI 创建 + 中间件（~8行）
├── 路由挂载（~2行）
├── WebSocket endpoint（~20行，调用 build_pipeline）
└── uvicorn 入口（~8行）
总计：~75行
```

---

## 7. 测试方案

### 7.1 策略

**纯回归验证**：不新增测试用例。所有现有测试（7个测试文件）迁移后必须全部通过。

### 7.2 迁移顺序（降低风险）

```
步骤 1：创建 tests/ + conftest.py
        → 移动 test_*.py，运行 pytest tests/ 确认全部通过

步骤 2：创建 core/ 包
        → 移动 latency.py / db.py / protocol.py / session.py
        → 更新 tests/ 中相关 imports
        → pytest tests/test_latency.py tests/test_db.py tests/test_protocol.py tests/test_session.py

步骤 3：创建 services/ 包
        → 移动 dashscope_services.py / services.py
        → 更新 imports
        → pytest tests/

步骤 4：创建 api/ 包
        → 移动 admin_api.py
        → 更新 imports
        → pytest tests/test_admin_api.py

步骤 5：创建 pipeline/ 包
        → 移动 pipeline.py（→ voice.py）
        → 从 main.py 提取 processors.py 和 builder.py
        → 更新 imports
        → pytest tests/

步骤 6：精简 main.py
        → 确认 < 80 行
        → make server 启动验证
        → pytest tests/test_e2e.py
```

### 7.3 验收检查命令

```bash
# 全量回归测试
cd pipecat-server && .venv/bin/pytest tests/ -v

# main.py 行数检查
wc -l pipecat-server/main.py

# 启动验证
make server
```

---

## 8. 影响评估

| 影响项 | 评估 |
|--------|------|
| 对外 WebSocket 协议 | 无变化 |
| Admin REST API | 无变化 |
| `make server` 命令 | 无变化（`main.py` 路径不变） |
| `make admin` 命令 | 无变化 |
| `.env` / `config.py` | 无变化 |
| SQLite 数据库文件 | 无变化（`db.py` 中 `DB_PATH` 基于 `__file__`，移动后路径需确认） |
| iOS 客户端 | 无影响 |

**`DB_PATH` 注意事项**：`core/db.py` 移动后，`os.path.dirname(__file__)` 将指向 `core/` 目录而非 `pipecat-server/`。需调整为：
```python
DB_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)), "voicemask.db")
```
即退出一层到 `pipecat-server/`，保持数据库文件位置不变。

---

## 9. 回滚方案

重构期间所有变更在 `feat/05-pipecat-server-refactor` 分支上进行，合并前经过完整测试验证。如需回滚，`git revert` merge commit 即可恢复所有文件到原始平铺结构。
