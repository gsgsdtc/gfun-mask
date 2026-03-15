# Module Spec: admin

> 模块：Admin 管理后台
> 最近同步：2026-03-13
> 状态：Phase 4 完成（对话监控 + 延迟分析前端）

---

## 1. 模块概述

独立的 Web 前端应用，用于查看语音对话历史、分析各环节延迟指标。通过调用 `pipecat-pipeline` 模块暴露的 Admin REST API 获取数据。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | `pipecat-pipeline` Admin REST API（`/api/admin/*`） |
| 下游 | 无（纯展示层） |
| 输入 | 浏览器 HTTP 请求 |
| 输出 | Web 页面（对话列表、详情、统计概览） |

### 1.2 技术选型

| 组件 | 技术 | 说明 |
|------|------|------|
| 构建工具 | Vite 8 | 开发服务器 + 构建 |
| UI 框架 | React 19 + TypeScript | 组件化页面开发 |
| 样式 | Tailwind CSS v4 | `@import "tailwindcss"` 语法，无需 config 文件 |
| 路由 | React Router v6 | 客户端路由，`Outlet` + `NavLink` |
| HTTP | Fetch API | 原生浏览器 API，无额外依赖 |
| API 代理 | Vite proxy | 开发时 `/api` → `http://localhost:8765` |

---

## 2. 页面结构

### 2.1 路由表

| 路径 | 页面 | 说明 |
|------|------|------|
| `/` | `Overview` | 概览：今日统计 + 平均延迟 + 最近 5 条对话 |
| `/conversations` | `Conversations` | 分页对话列表 |
| `/conversations/:id` | `ConversationDetail` | 单条对话详情（含延迟分解 + 时间轴） |

### 2.2 布局

- 顶部导航栏：品牌名 + 当前页面标题
- 左侧边栏：`概览` / `对话记录` 导航，`NavLink` 高亮当前页
- 主内容区：`<Outlet />` 渲染各页面

---

## 3. API 层（src/api/admin.ts）

### 3.1 数据类型

```typescript
interface StatsResponse {
  today_count: number
  avg_e2e_ttfa_ms: number | null
  avg_asr_total_ms: number | null
  avg_llm_ttft_ms: number | null
  avg_tts_ttfa_ms: number | null
  recent: ConversationSummary[]
}

interface ConversationSummary {
  id: number
  created_at: string
  user_text: string
  ai_text: string
  e2e_ttfa_ms: number | null
  e2e_total_ms: number | null
}

interface ConversationDetail {
  id: number
  session_id: string
  created_at: string
  user_text: string
  ai_text: string
  asr_ttfa_ms: number | null
  asr_total_ms: number | null
  llm_ttft_ms: number | null
  llm_total_ms: number | null
  tts_ttfa_ms: number | null
  tts_total_ms: number | null
  e2e_ttfa_ms: number | null
  e2e_total_ms: number | null
}
```

### 3.2 API 函数

| 函数 | 请求 | 说明 |
|------|------|------|
| `api.getStats()` | `GET /api/admin/stats` | 获取概览统计 |
| `api.getConversations(page, size)` | `GET /api/admin/conversations?page=&size=` | 分页列表 |
| `api.getConversation(id)` | `GET /api/admin/conversations/:id` | 单条详情 |

---

## 4. 页面功能

### 4.1 Overview（概览）

- 4 个统计卡片：今日对话数 / 平均 E2E 首包 / 平均 ASR / 平均 LLM TTFT
- 最近 5 条对话表格：时间 / 用户说 / AI 回复 / 首包时间（`>1000ms` 红色）
- 骨架屏加载态（`animate-pulse`）

### 4.2 Conversations（对话列表）

- 分页表格（每页 20 条，URL `?page=N` 保持状态）
- 列：时间 / 用户说（截断 40 字）/ AI 回复（截断 40 字）/ 首包时间 / 总时间
- `e2e_ttfa_ms > 1000ms` 红色显示
- 点击行跳转详情页

### 4.3 ConversationDetail（对话详情）

- 对话气泡：用户（蓝色背景）/ AI（灰色背景）
- 耗时分解表格：ASR / LLM / TTS 各环节首包时间 + 总时间 + 整体首包合计
- 首包时间轴（CSS 横条）：ASR / LLM / TTS 色块按首包时间在 E2E 中的占比排布

---

## 5. 配置

| 参数 | 值 | 说明 |
|------|----|------|
| dev server host | `0.0.0.0` | 局域网可访问 |
| dev server port | `5173`（Vite 默认） | 开发服务器端口 |
| API proxy | `/api` → `http://localhost:8765` | 仅开发环境 |

启动命令：`npm run dev`（对应 `make admin`）

---

## 6. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-13 | feat #04 | 初始实现：Vite + React + Tailwind，三页面（概览/列表/详情） |
| 2026-03-13 | feat #04 | 新增总时间（e2e_total_ms）列到对话列表 |
| 2026-03-13 | fix | dev server 绑定 0.0.0.0，支持局域网访问 |
