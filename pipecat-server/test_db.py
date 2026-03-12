"""
@doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.1, §3.1
@purpose 验证 db.py 的 SQLite CRUD 操作正确性（使用 in-memory DB）
@context db.py 是所有对话记录的持久化层；若 insert/query/pagination 有误，
         Admin 后台将显示错误或缺失的数据，导致性能分析失效。
@depends db.init_db, db.insert_conversation, db.get_conversations, db.get_conversation_by_id, db.get_stats
"""

import pytest
import pytest_asyncio
import aiosqlite
from datetime import datetime, timezone


@pytest_asyncio.fixture
async def db():
    """每个测试使用独立的 in-memory SQLite 连接。"""
    import db as db_module
    conn = await aiosqlite.connect(":memory:")
    conn.row_factory = aiosqlite.Row
    await db_module.init_db(conn)
    yield conn
    await conn.close()


class TestInitDb:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §4.3
    # @purpose 验证 init_db 创建 conversations 表和索引
    # @context 服务启动时调用 init_db；若表未创建，所有写入操作都会报错导致服务崩溃
    @pytest.mark.asyncio
    async def test_creates_conversations_table(self, db):
        async with db.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name='conversations'"
        ) as cur:
            row = await cur.fetchone()
        assert row is not None


class TestInsertConversation:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.1
    # @purpose 验证插入一条完整对话记录后可被查询到
    # @context insert_conversation 是每次语音请求结束的核心写入操作；
    #          若写入失败或字段丢失，数据库将缺少关键性能数据
    @pytest.mark.asyncio
    async def test_insert_and_retrieve(self, db):
        import db as db_module
        row_id = await db_module.insert_conversation(db, {
            "session_id": "sess-001",
            "created_at": "2026-03-13T10:00:00Z",
            "user_text": "你好",
            "ai_text": "你好！",
            "asr_ttfa_ms": 300,
            "asr_total_ms": 300,
            "llm_ttft_ms": 450,
            "llm_total_ms": 1100,
            "tts_ttfa_ms": 200,
            "tts_total_ms": 850,
            "e2e_ttfa_ms": 950,
        })
        assert row_id == 1

        row = await db_module.get_conversation_by_id(db, 1)
        assert row["user_text"] == "你好"
        assert row["asr_total_ms"] == 300
        assert row["e2e_ttfa_ms"] == 950

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.1
    # @purpose 验证部分字段为 None 时可正常插入（如 LLM 未完成）
    # @context 管道中间出错时部分计时字段可能为 None；必须允许 null 插入否则会丢失数据
    @pytest.mark.asyncio
    async def test_insert_with_null_fields(self, db):
        import db as db_module
        row_id = await db_module.insert_conversation(db, {
            "session_id": "sess-002",
            "created_at": "2026-03-13T10:01:00Z",
            "user_text": "测试",
            "ai_text": "",
            "asr_ttfa_ms": None,
            "asr_total_ms": None,
            "llm_ttft_ms": None,
            "llm_total_ms": None,
            "tts_ttfa_ms": None,
            "tts_total_ms": None,
            "e2e_ttfa_ms": None,
        })
        row = await db_module.get_conversation_by_id(db, row_id)
        assert row["e2e_ttfa_ms"] is None


class TestGetConversations:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证分页查询返回正确的 total / items / 倒序排列
    # @context Admin 对话列表页依赖分页接口；若 total 错误或排序反向，用户看到的数据顺序混乱
    @pytest.mark.asyncio
    async def test_pagination_and_order(self, db):
        import db as db_module
        for i in range(5):
            await db_module.insert_conversation(db, {
                "session_id": f"sess-{i}",
                "created_at": f"2026-03-13T10:0{i}:00Z",
                "user_text": f"msg {i}", "ai_text": f"reply {i}",
                "asr_ttfa_ms": 300, "asr_total_ms": 300,
                "llm_ttft_ms": 450, "llm_total_ms": 1100,
                "tts_ttfa_ms": 200, "tts_total_ms": 850, "e2e_ttfa_ms": 950,
            })

        result = await db_module.get_conversations(db, page=1, size=3)
        assert result["total"] == 5
        assert len(result["items"]) == 3
        # 倒序：最新的在前（created_at DESC）
        assert result["items"][0]["user_text"] == "msg 4"

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证第 2 页返回剩余的 2 条记录
    # @context 分页偏移计算错误会导致某些记录永远不出现在列表中
    @pytest.mark.asyncio
    async def test_second_page(self, db):
        import db as db_module
        for i in range(5):
            await db_module.insert_conversation(db, {
                "session_id": f"sess-{i}",
                "created_at": f"2026-03-13T10:0{i}:00Z",
                "user_text": f"msg {i}", "ai_text": "",
                "asr_ttfa_ms": None, "asr_total_ms": None,
                "llm_ttft_ms": None, "llm_total_ms": None,
                "tts_ttfa_ms": None, "tts_total_ms": None, "e2e_ttfa_ms": None,
            })
        result = await db_module.get_conversations(db, page=2, size=3)
        assert len(result["items"]) == 2


class TestGetStats:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 get_stats 返回今日对话数和各环节平均耗时
    # @context Admin 概览页的统计卡片依赖此接口；数据不准确会误导开发者的优化决策
    @pytest.mark.asyncio
    async def test_stats_today_count_and_avg(self, db):
        import db as db_module
        today = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        for _ in range(3):
            await db_module.insert_conversation(db, {
                "session_id": "s", "created_at": today,
                "user_text": "hi", "ai_text": "hello",
                "asr_ttfa_ms": 300, "asr_total_ms": 300,
                "llm_ttft_ms": 400, "llm_total_ms": 1000,
                "tts_ttfa_ms": 200, "tts_total_ms": 800, "e2e_ttfa_ms": 900,
            })
        stats = await db_module.get_stats(db)
        assert stats["today_count"] == 3
        assert stats["avg_asr_total_ms"] == 300
        assert stats["avg_llm_ttft_ms"] == 400
        assert stats["avg_e2e_ttfa_ms"] == 900

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 recent 返回最近 5 条
    # @context 概览页展示最近 5 条对话；若超出 5 条会使页面布局溢出
    @pytest.mark.asyncio
    async def test_stats_recent_max_5(self, db):
        import db as db_module
        today = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        for i in range(7):
            await db_module.insert_conversation(db, {
                "session_id": f"s{i}", "created_at": today,
                "user_text": f"hi{i}", "ai_text": "hello",
                "asr_ttfa_ms": 300, "asr_total_ms": 300,
                "llm_ttft_ms": 400, "llm_total_ms": 1000,
                "tts_ttfa_ms": 200, "tts_total_ms": 800, "e2e_ttfa_ms": 900,
            })
        stats = await db_module.get_stats(db)
        assert len(stats["recent"]) == 5


class TestGetConversationById:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
    # @purpose 验证 id 不存在时返回 None（而非抛出异常）
    # @context admin_api.py 需要判断 None 并返回 404；若抛出异常会变成 500 错误
    @pytest.mark.asyncio
    async def test_not_found_returns_none(self, db):
        import db as db_module
        row = await db_module.get_conversation_by_id(db, 999)
        assert row is None
