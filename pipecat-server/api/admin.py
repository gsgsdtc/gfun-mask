"""
@doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §3.1
@purpose Admin REST API：/api/admin/stats, /api/admin/conversations, /api/admin/conversations/{id}
"""

from __future__ import annotations

from typing import AsyncGenerator

import aiosqlite
from fastapi import APIRouter, Depends, HTTPException

from core import db as db_module

router = APIRouter(prefix="/api/admin")


async def get_db() -> AsyncGenerator[aiosqlite.Connection, None]:
    """FastAPI 依赖：打开文件数据库连接，请求结束后关闭。"""
    conn = await db_module.get_connection()
    try:
        yield conn
    finally:
        await conn.close()


@router.get("/stats")
async def stats(conn: aiosqlite.Connection = Depends(get_db)):
    return await db_module.get_stats(conn)


@router.get("/conversations")
async def conversations(
    page: int = 1,
    size: int = 20,
    conn: aiosqlite.Connection = Depends(get_db),
):
    return await db_module.get_conversations(conn, page=page, size=size)


@router.get("/conversations/{conversation_id}")
async def conversation_detail(
    conversation_id: int,
    conn: aiosqlite.Connection = Depends(get_db),
):
    row = await db_module.get_conversation_by_id(conn, conversation_id)
    if row is None:
        raise HTTPException(status_code=404, detail="Conversation not found")
    return row
