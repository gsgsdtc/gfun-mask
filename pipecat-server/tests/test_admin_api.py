"""
@doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
@purpose 验证 /api/admin/* REST 端点的响应结构、分页、404 处理
@context admin_api.py 是 Admin 后台前端的数据来源；
         若响应结构不符合约定，前端 fetch 解析失败，页面将无法渲染数据。
@depends admin_api.router, db.init_db, db.insert_conversation
"""

import pytest
import pytest_asyncio
import aiosqlite
from datetime import datetime, timezone
from fastapi import FastAPI
from fastapi.testclient import TestClient
from unittest.mock import AsyncMock, patch, MagicMock


def _make_record(i: int, today: str) -> dict:
    return {
        "session_id": f"sess-{i}",
        "created_at": f"{today}T10:0{i % 6}:00Z",
        "user_text": f"用户消息 {i}",
        "ai_text": f"AI 回复 {i}",
        "asr_ttfa_ms": 300, "asr_total_ms": 300,
        "llm_ttft_ms": 450, "llm_total_ms": 1100,
        "tts_ttfa_ms": 200, "tts_total_ms": 850,
        "e2e_ttfa_ms": 950, "e2e_total_ms": 1200,
    }


@pytest.fixture
def client_with_db():
    """
    创建临时 in-memory DB + FastAPI TestClient。
    将 admin_api 中的 get_db_conn 替换为返回内存 DB 的依赖。
    """
    import asyncio
    from core import db as db_module
    from api import admin as admin_api

    loop = asyncio.new_event_loop()
    conn = loop.run_until_complete(aiosqlite.connect(":memory:"))
    conn.row_factory = aiosqlite.Row
    loop.run_until_complete(db_module.init_db(conn))

    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    for i in range(7):
        loop.run_until_complete(db_module.insert_conversation(conn, _make_record(i, today)))

    async def override_get_db():
        yield conn

    app = FastAPI()
    app.include_router(admin_api.router)
    app.dependency_overrides[admin_api.get_db] = override_get_db

    with TestClient(app) as c:
        yield c

    loop.run_until_complete(conn.close())
    loop.close()


class TestStatsEndpoint:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 /api/admin/stats 返回 200 且包含必需字段
    # @context 概览页首屏依赖此接口；若字段缺失 JS 解析失败，页面显示空白
    def test_stats_returns_required_fields(self, client_with_db):
        resp = client_with_db.get("/api/admin/stats")
        assert resp.status_code == 200
        data = resp.json()
        assert "today_count" in data
        assert "avg_e2e_ttfa_ms" in data
        assert "avg_asr_total_ms" in data
        assert "avg_llm_ttft_ms" in data
        assert "recent" in data
        assert isinstance(data["recent"], list)

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 recent 最多 5 条（即使 DB 有 7 条）
    # @context 概览页布局固定展示 5 条；超过会破坏页面样式
    def test_stats_recent_max_5(self, client_with_db):
        data = client_with_db.get("/api/admin/stats").json()
        assert len(data["recent"]) <= 5


class TestConversationsEndpoint:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 /api/admin/conversations 返回分页结构
    # @context 对话列表页依赖 total/page/size/items 字段；缺少任一字段分页控件将无法渲染
    def test_returns_pagination_structure(self, client_with_db):
        resp = client_with_db.get("/api/admin/conversations?page=1&size=5")
        assert resp.status_code == 200
        data = resp.json()
        assert data["total"] == 7
        assert data["page"] == 1
        assert data["size"] == 5
        assert len(data["items"]) == 5

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证第 2 页返回剩余 2 条
    # @context 分页偏移错误会导致最后一页数据丢失
    def test_second_page_returns_remaining(self, client_with_db):
        data = client_with_db.get("/api/admin/conversations?page=2&size=5").json()
        assert len(data["items"]) == 2

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 items 中包含必要展示字段
    # @context 前端列表页需要 id/created_at/user_text/ai_text/e2e_ttfa_ms 渲染每行
    def test_items_contain_required_fields(self, client_with_db):
        data = client_with_db.get("/api/admin/conversations").json()
        item = data["items"][0]
        for key in ("id", "created_at", "user_text", "ai_text", "e2e_ttfa_ms"):
            assert key in item


class TestConversationDetailEndpoint:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 /api/admin/conversations/{id} 返回完整耗时字段
    # @context 详情页需要所有延迟字段渲染耗时分解面板和时间轴
    def test_returns_full_latency_fields(self, client_with_db):
        resp = client_with_db.get("/api/admin/conversations/1")
        assert resp.status_code == 200
        data = resp.json()
        for key in (
            "id", "session_id", "created_at", "user_text", "ai_text",
            "asr_ttfa_ms", "asr_total_ms",
            "llm_ttft_ms", "llm_total_ms",
            "tts_ttfa_ms", "tts_total_ms",
            "e2e_ttfa_ms",
        ):
            assert key in data

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 id 不存在时返回 404
    # @context 前端直接访问不存在的 id 必须得到 404 而非 500，以便正确展示"未找到"页面
    def test_not_found_returns_404(self, client_with_db):
        resp = client_with_db.get("/api/admin/conversations/9999")
        assert resp.status_code == 404
