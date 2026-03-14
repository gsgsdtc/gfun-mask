# Feat-05: pipecat-server 目录结构重构

> 创建时间：2026-03-14
> 来源：内部技术债务治理
> 优先级：中（不阻塞业务功能，但影响后续所有迭代效率）

---

## 1. 背景与目标

### 1.1 当前问题

`pipecat-server/` 目前是完全平铺的单目录结构，所有 Python 文件（业务代码、协议处理、服务封装、数据库、API、计时系统、测试）混放在同一层级，共 17 个文件。

随着 feat-03、feat-04 的迭代，文件数量持续增长，已出现以下问题：

- **职责不清晰**：`main.py` 同时承担 FastAPI 应用初始化、WebSocket endpoint、Pipecat 管道编排、多个 FrameProcessor 定义，单文件超过 400 行
- **测试与业务混杂**：`test_*.py` 与业务代码平铺在一起，无法一眼区分
- **依赖关系隐式**：各模块间的依赖关系只能通过阅读代码才能理解
- **后续扩展困难**：新增服务（如新的 STT/TTS 提供商）、新的 FrameProcessor、新的 API endpoint 都没有明确的落点

### 1.2 重构目标

- 建立清晰的分层目录结构，明确每一层的职责边界
- 测试文件集中管理，与业务代码分离
- `main.py` 瘦身为纯粹的应用入口（组装 + 启动）
- 为后续迭代（新服务接入、新 API、新处理器）提供明确的落点

### 1.3 非目标

- 不修改任何功能逻辑
- 不变更对外协议（WebSocket 接口、Admin REST API）
- 不升级依赖版本
- 不重构 admin-web 前端

---

## 2. 使用场景

### 场景 A：开发者新增 STT 提供商

重构后，开发者清楚地知道应在 `services/` 目录下新建文件，遵循已有服务的接口约定，而不是在 `main.py` 或 `dashscope_services.py` 中扩展。

### 场景 B：新增 FrameProcessor

重构后，所有管道处理器集中在 `pipeline/` 目录，新增处理器有明确落点，不需要在 `main.py` 里找位置插入。

### 场景 C：新增 Admin API endpoint

重构后，API 路由集中在 `api/` 目录，新增 endpoint 只需在对应文件中添加，无需理解整个 `main.py`。

### 场景 D：运行测试

重构后，所有测试集中在 `tests/` 目录，一条命令即可运行，无需过滤平铺目录中的非测试文件。

---

## 3. 期望的目录结构

重构后 `pipecat-server/` 应形成以下分层：

```
pipecat-server/
├── main.py              # 应用入口：FastAPI 创建、中间件、路由挂载、uvicorn 启动
├── config.py            # 环境配置（不变）
├── api/                 # HTTP API 层
│   └── admin.py         # Admin REST 路由（原 admin_api.py）
├── pipeline/            # Pipecat 管道层：处理器定义 + 管道组装
│   ├── processors.py    # FrameProcessor 子类（Protocol/Transcript/LLMCapture/TTS 转发器等）
│   └── builder.py       # 管道组装函数（build_pipeline）
├── services/            # 外部服务封装
│   └── dashscope.py     # DashScope STT + TTS（原 dashscope_services.py）
├── core/                # 核心基础设施
│   ├── latency.py       # 延迟计量（原 latency.py）
│   └── db.py            # SQLite 持久化（原 db.py）
└── tests/               # 所有测试文件
    ├── test_latency.py
    ├── test_db.py
    ├── test_admin_api.py
    └── ...
```

> 具体的文件命名和子目录划分由技术设计阶段确定，以上为参考方向。

---

## 4. 约束条件

- 重构后所有现有测试必须通过（零回归）
- `main.py` 入口文件路径保持不变（`make server` 命令不受影响）
- 现有 `.env` 配置和 `config.py` 读取方式不变
- Python import 路径调整后，模块内部的相对/绝对引用需保持一致

---

## 5. 验收标准

- [ ] `pipecat-server/` 根目录下不存在业务逻辑文件（仅保留 `main.py`、`config.py` 及包目录）
- [ ] 所有测试文件集中在 `tests/` 目录，`pytest tests/` 全部通过
- [ ] `main.py` 行数不超过 80 行（仅包含应用创建、中间件、路由挂载、uvicorn 入口）
- [ ] `make server` 正常启动，WebSocket 端点和 Admin API 均可访问
- [ ] 新建一个 FrameProcessor 时，只需在 `pipeline/processors.py` 中添加，不需要修改 `main.py`
- [ ] 新建一个 Admin API endpoint 时，只需在 `api/admin.py` 中添加，不需要修改 `main.py`
