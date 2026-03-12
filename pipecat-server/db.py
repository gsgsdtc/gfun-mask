"""
@doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.1, §4.3
@purpose SQLite 持久化层：初始化、对话记录 CRUD、统计查询
"""

from __future__ import annotations

import os
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import aiosqlite
from loguru import logger

DB_PATH = os.path.join(os.path.dirname(__file__), "voicemask.db")

_CREATE_TABLE = """
CREATE TABLE IF NOT EXISTS conversations (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id   TEXT    NOT NULL,
    created_at   TEXT    NOT NULL,
    user_text    TEXT    NOT NULL DEFAULT '',
    ai_text      TEXT    NOT NULL DEFAULT '',
    asr_ttfa_ms  INTEGER,
    asr_total_ms INTEGER,
    llm_ttft_ms  INTEGER,
    llm_total_ms INTEGER,
    tts_ttfa_ms  INTEGER,
    tts_total_ms INTEGER,
    e2e_ttfa_ms  INTEGER
);
"""
_CREATE_INDEX = "CREATE INDEX IF NOT EXISTS idx_created_at ON conversations(created_at DESC);"


async def init_db(conn: aiosqlite.Connection) -> None:
    """建表 + 建索引，幂等。"""
    await conn.execute(_CREATE_TABLE)
    await conn.execute(_CREATE_INDEX)
    await conn.commit()
    logger.debug("[DB] conversations 表已就绪")


async def get_connection() -> aiosqlite.Connection:
    """打开文件数据库连接（row_factory 已设置）。"""
    conn = await aiosqlite.connect(DB_PATH)
    conn.row_factory = aiosqlite.Row
    return conn


async def insert_conversation(conn: aiosqlite.Connection, data: Dict[str, Any]) -> int:
    """插入一条对话记录，返回新行 id。"""
    cur = await conn.execute(
        """
        INSERT INTO conversations
            (session_id, created_at, user_text, ai_text,
             asr_ttfa_ms, asr_total_ms,
             llm_ttft_ms, llm_total_ms,
             tts_ttfa_ms, tts_total_ms,
             e2e_ttfa_ms)
        VALUES
            (:session_id, :created_at, :user_text, :ai_text,
             :asr_ttfa_ms, :asr_total_ms,
             :llm_ttft_ms, :llm_total_ms,
             :tts_ttfa_ms, :tts_total_ms,
             :e2e_ttfa_ms)
        """,
        data,
    )
    await conn.commit()
    return cur.lastrowid


async def get_conversations(
    conn: aiosqlite.Connection, page: int = 1, size: int = 20
) -> Dict[str, Any]:
    """分页查询对话列表，按 created_at DESC 排序。"""
    size = min(size, 100)
    offset = (page - 1) * size

    async with conn.execute("SELECT COUNT(*) FROM conversations") as cur:
        total = (await cur.fetchone())[0]

    async with conn.execute(
        """
        SELECT id, created_at, user_text, ai_text, e2e_ttfa_ms
        FROM conversations
        ORDER BY created_at DESC
        LIMIT ? OFFSET ?
        """,
        (size, offset),
    ) as cur:
        rows = await cur.fetchall()

    return {
        "total": total,
        "page": page,
        "size": size,
        "items": [dict(r) for r in rows],
    }


async def get_conversation_by_id(
    conn: aiosqlite.Connection, row_id: int
) -> Optional[Dict[str, Any]]:
    """按 id 查询单条完整记录，不存在返回 None。"""
    async with conn.execute(
        "SELECT * FROM conversations WHERE id = ?", (row_id,)
    ) as cur:
        row = await cur.fetchone()
    return dict(row) if row else None


async def get_stats(conn: aiosqlite.Connection) -> Dict[str, Any]:
    """返回概览统计：今日对话数、各环节平均耗时、最近 5 条。"""
    today_prefix = datetime.now(timezone.utc).strftime("%Y-%m-%d")

    async with conn.execute(
        "SELECT COUNT(*) FROM conversations WHERE created_at LIKE ?",
        (f"{today_prefix}%",),
    ) as cur:
        today_count = (await cur.fetchone())[0]

    async with conn.execute(
        """
        SELECT
            ROUND(AVG(e2e_ttfa_ms))  AS avg_e2e_ttfa_ms,
            ROUND(AVG(asr_total_ms)) AS avg_asr_total_ms,
            ROUND(AVG(llm_ttft_ms))  AS avg_llm_ttft_ms,
            ROUND(AVG(tts_ttfa_ms))  AS avg_tts_ttfa_ms
        FROM conversations
        """
    ) as cur:
        avgs = dict(await cur.fetchone())

    async with conn.execute(
        """
        SELECT id, created_at, user_text, ai_text, e2e_ttfa_ms
        FROM conversations
        ORDER BY created_at DESC
        LIMIT 5
        """
    ) as cur:
        recent = [dict(r) for r in await cur.fetchall()]

    return {
        "today_count": today_count,
        "avg_e2e_ttfa_ms": int(avgs["avg_e2e_ttfa_ms"]) if avgs["avg_e2e_ttfa_ms"] else None,
        "avg_asr_total_ms": int(avgs["avg_asr_total_ms"]) if avgs["avg_asr_total_ms"] else None,
        "avg_llm_ttft_ms": int(avgs["avg_llm_ttft_ms"]) if avgs["avg_llm_ttft_ms"] else None,
        "avg_tts_ttfa_ms": int(avgs["avg_tts_ttfa_ms"]) if avgs["avg_tts_ttfa_ms"] else None,
        "recent": recent,
    }
