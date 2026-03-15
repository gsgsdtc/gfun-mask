"""
@doc     docs/modules/pipecat-pipeline/design/05-pipecat-server-refactor-backend-design.md §6
@purpose 应用入口：FastAPI 创建 + 中间件 + 路由挂载 + lifespan + uvicorn
"""

from __future__ import annotations

import sys
from contextlib import asynccontextmanager
from pathlib import Path

import uvicorn
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.websockets import WebSocket
from pipecat.pipeline.runner import PipelineRunner

from api import admin as admin_api
from core import db as db_module
from core.latency import LatencyRecord
from pipeline.builder import build_pipeline, make_on_complete

# ── 日志配置：在所有 pipecat 导入完成后设置 ──
from loguru import logger

_LOG_DIR = Path(__file__).parent / "logs"
_LOG_DIR.mkdir(exist_ok=True)
logger.remove()
logger.add(sys.stderr, level="DEBUG", colorize=True,
           format="<green>{time:HH:mm:ss.SSS}</green> | <level>{level: <8}</level> | {message}")
logger.add(_LOG_DIR / "server.log", level="DEBUG", rotation="10 MB", retention=5,
           encoding="utf-8",
           format="{time:YYYY-MM-DD HH:mm:ss.SSS} | {level: <8} | {name}:{function}:{line} - {message}")
logger.info(f"[Main] 日志已初始化，文件路径: {_LOG_DIR / 'server.log'}")


@asynccontextmanager
async def lifespan(app: FastAPI):
    conn = await db_module.get_connection()
    try:
        await db_module.init_db(conn)
    finally:
        await conn.close()
    logger.info("[Main] 数据库初始化完成")
    yield


app = FastAPI(title="VoiceMask Pipecat Server", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173", "http://127.0.0.1:5173"],
    allow_methods=["GET"],
    allow_headers=["*"],
)
app.include_router(admin_api.router)


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    record = LatencyRecord()
    task = await build_pipeline(websocket, record, make_on_complete(db_module))
    runner = PipelineRunner(handle_sigint=False)
    await runner.run(task)


if __name__ == "__main__":
    from config import Config
    uvicorn.run("main:app", host=Config.SERVER_HOST, port=Config.SERVER_PORT, log_level="info")
