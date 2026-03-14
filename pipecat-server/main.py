"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md
@purpose 基于 Pipecat 框架的语音聊天服务
         使用 FastAPIWebsocketTransport + 自定义 iOS 协议序列化器
         STT: DashScope Paraformer，LLM: Qwen，TTS: DashScope CosyVoice
"""

from __future__ import annotations

import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import uvicorn
from contextlib import asynccontextmanager
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.websockets import WebSocket

from pipecat.frames.frames import (
    AudioRawFrame,
    EndFrame,
    ErrorFrame,
    Frame,
    InputAudioRawFrame,
    LLMFullResponseEndFrame,
    OutputTransportMessageUrgentFrame,
    TTSAudioRawFrame,
    TTSStartedFrame,
    TTSStoppedFrame,
    TranscriptionFrame,
    TextFrame,
    StartFrame,
    UserStartedSpeakingFrame,
    UserStoppedSpeakingFrame,
    VADUserStartedSpeakingFrame,
    VADUserStoppedSpeakingFrame,
)
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import LLMContextAggregatorPair
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor
from pipecat.serializers.base_serializer import FrameSerializer
from pipecat.services.qwen.llm import QwenLLMService
from pipecat.transports.websocket.fastapi import (
    FastAPIWebsocketParams,
    FastAPIWebsocketTransport,
)
from config import Config, DASHSCOPE_BASE_URL
from dashscope_services import DashScopeSTTService, DashScopeTTSService
from latency import LatencyRecord, LatencyTracker
import db as db_module
import admin_api

# ── 日志配置：在所有 pipecat 导入完成后设置，避免被 pipecat 内部 logger.remove() 覆盖 ──
from loguru import logger

_LOG_DIR = Path(__file__).parent / "logs"
_LOG_DIR.mkdir(exist_ok=True)

logger.remove()
logger.add(sys.stderr, level="DEBUG", colorize=True,
           format="<green>{time:HH:mm:ss.SSS}</green> | <level>{level: <8}</level> | {message}")
logger.add(
    _LOG_DIR / "server.log",
    level="DEBUG",
    rotation="10 MB",
    retention=5,
    encoding="utf-8",
    format="{time:YYYY-MM-DD HH:mm:ss.SSS} | {level: <8} | {name}:{function}:{line} - {message}",
)

logger.info(f"[Main] 日志已初始化，文件路径: {_LOG_DIR / 'server.log'}")

# ──────────────────────────────────────────────
# 自定义帧：iOS ping 控制消息
# ──────────────────────────────────────────────

class iOSPingFrame(Frame):
    pass

# ──────────────────────────────────────────────
# iOS 协议序列化器
# 将 iOS WebSocket 消息 ↔ Pipecat Frame 互转
# ──────────────────────────────────────────────

TTS_AUDIO_PREFIX = 0xAA


class iOSProtocolSerializer(FrameSerializer):
    """
    iOS ↔ Pipecat 协议转换：
    - 反序列化：iOS JSON/binary → Pipecat Frame
    - 序列化：只处理 OutputTransportMessageUrgentFrame（其他帧由 TTSAudioForwarder 包装）
    """

    async def serialize(self, frame: Frame) -> str | bytes | None:
        """将 Pipecat 内部帧转为 iOS 期望的 WebSocket 消息格式"""
        if isinstance(frame, OutputTransportMessageUrgentFrame):
            logger.debug(f"[Serializer] → urgent: {type(frame.message).__name__} {len(frame.message) if isinstance(frame.message, (bytes, str)) else ''}")
            return frame.message
        return None

    async def deserialize(self, data: str | bytes) -> Frame | None:
        """将 iOS WebSocket 消息转为 Pipecat 内部帧"""
        if isinstance(data, bytes):
            # 二进制帧：原始 PCM 音频 → Pipecat 原生 InputAudioRawFrame
            return InputAudioRawFrame(audio=data, sample_rate=16000, num_channels=1)

        # 文本帧：JSON 控制消息
        try:
            msg = json.loads(data)
        except json.JSONDecodeError:
            return None

        msg_type = msg.get("type", "")
        if msg_type == "start":
            logger.info("[Serializer] ← start → VADUserStartedSpeakingFrame")
            return VADUserStartedSpeakingFrame()
        elif msg_type == "stop":
            logger.info("[Serializer] ← stop → VADUserStoppedSpeakingFrame")
            return VADUserStoppedSpeakingFrame()
        elif msg_type == "ping":
            return iOSPingFrame()
        return None


# ──────────────────────────────────────────────
# Ping 处理器：回送 pong，其余帧直接透传
# ──────────────────────────────────────────────

class PingHandler(FrameProcessor):
    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)
        if isinstance(frame, iOSPingFrame):
            logger.debug("[PingHandler] ping → pong")
            pong = OutputTransportMessageUrgentFrame(message=json.dumps({"type": "pong"}))
            await self.push_frame(pong)
        else:
            await self.push_frame(frame, direction)


# ──────────────────────────────────────────────
# STT 转录转发器
# 放在 context_aggregator.user() 之前，拦截 TranscriptionFrame。
# context_aggregator.user() 会消费 TranscriptionFrame（不继续下传），
# 因此必须在它之前将 transcript_final 发给 iOS。
# TranscriptionFrame 仍继续传递，让 context_aggregator.user() 正常工作。
# ──────────────────────────────────────────────

class TranscriptForwarder(FrameProcessor):
    def __init__(self, record: LatencyRecord, **kwargs):
        super().__init__(**kwargs)
        self._record = record

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)
        if isinstance(frame, TranscriptionFrame):
            self._record.user_text = frame.text
            msg = json.dumps({"type": "transcript_final", "text": frame.text})
            logger.info(f"[TranscriptForwarder] → transcript_final: '{frame.text}'")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=msg))
            await self.push_frame(frame, direction)  # 继续传递给 context_aggregator.user()
        elif isinstance(frame, ErrorFrame):
            msg = json.dumps({"type": "error", "code": "STT_EMPTY", "message": str(frame.error)})
            logger.warning(f"[TranscriptForwarder] → error: {frame.error}")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=msg))
            await self.push_frame(frame, direction)
        else:
            await self.push_frame(frame, direction)


# ──────────────────────────────────────────────
# LLM 文本捕获器
# 放在 llm 和 tts 之间，在 TTS 消费 TextFrame 之前拦截。
# 负责：llm_ttft / llm_end 计时、ai_text 累积、llm_done 消息发送。
# ──────────────────────────────────────────────

class LLMTextCapture(FrameProcessor):
    def __init__(self, record: LatencyRecord, **kwargs):
        super().__init__(**kwargs)
        self._record = record
        self._buffer: list[str] = []

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)
        if isinstance(frame, TextFrame):
            if self._record.llm_ttft is None:
                self._record.llm_ttft = time.monotonic()
            self._buffer.append(frame.text)
        elif isinstance(frame, LLMFullResponseEndFrame):
            self._record.llm_end = time.monotonic()
            full_text = "".join(self._buffer)
            self._record.ai_text = full_text
            self._buffer.clear()
            if full_text:
                msg = json.dumps({"type": "llm_done", "text": full_text})
                logger.info(f"[LLMCapture] → llm_done: '{full_text[:60]}' ({len(full_text)} chars)")
                await self.push_frame(OutputTransportMessageUrgentFrame(message=msg))
        await self.push_frame(frame, direction)


# ──────────────────────────────────────────────
# TTS 帧转发器
# 放在 tts 之后，处理 TTS 音频帧。
# Pipecat 传输层只处理 OutputTransportMessageUrgentFrame，
# 其他帧会被静默丢弃，此处统一包装。
# ──────────────────────────────────────────────

class TTSAudioForwarder(FrameProcessor):
    def __init__(self, record: LatencyRecord, **kwargs):
        super().__init__(**kwargs)
        self._record = record
        self._tts_audio_received = False

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)
        if isinstance(frame, TTSAudioRawFrame):
            if not self._tts_audio_received:
                self._record.tts_ttfa = time.monotonic()
                self._tts_audio_received = True
            data = bytes([TTS_AUDIO_PREFIX]) + frame.audio
            logger.debug(f"[TTSForwarder] → tts_audio binary: {len(frame.audio)} bytes MP3")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=data))
        elif isinstance(frame, TTSStartedFrame):
            logger.info("[TTSForwarder] → tts_start")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=json.dumps({"type": "tts_start"})))
        elif isinstance(frame, TTSStoppedFrame):
            self._record.tts_end = time.monotonic()
            self._tts_audio_received = False  # 重置，为下一轮准备
            logger.info("[TTSForwarder] → tts_end")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=json.dumps({"type": "tts_end"})))
            if self._record.on_complete:
                await self._record.on_complete(self._record)
        else:
            await self.push_frame(frame, direction)


# ──────────────────────────────────────────────
# FastAPI 应用
# ──────────────────────────────────────────────

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
    transport = FastAPIWebsocketTransport(
        websocket=websocket,
        params=FastAPIWebsocketParams(
            audio_in_enabled=True,    # 必须 True，否则 push_audio_frame 丢弃 InputAudioRawFrame
            audio_out_enabled=True,   # 需要 True 以便 TTSStartedFrame/TTSStoppedFrame 正常传递
            serializer=iOSProtocolSerializer(),
        ),
    )

    # LLM：通义千问（DashScope OpenAI 兼容接口）
    llm = QwenLLMService(
        api_key=Config.DASHSCOPE_API_KEY,
        base_url=DASHSCOPE_BASE_URL,
        model=Config.LLM_MODEL,
    )

    # 延迟记录（本次会话共享）
    record = LatencyRecord()

    async def _on_complete(rec: LatencyRecord) -> None:
        rec.emit_log()
        data = {
            "session_id": rec.session_id,
            "created_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "user_text": rec.user_text,
            "ai_text": rec.ai_text,
            "asr_ttfa_ms": rec.asr_ttfa_ms,
            "asr_total_ms": rec.asr_total_ms,
            "llm_ttft_ms": rec.llm_ttft_ms,
            "llm_total_ms": rec.llm_total_ms,
            "tts_ttfa_ms": rec.tts_ttfa_ms,
            "tts_total_ms": rec.tts_total_ms,
            "e2e_ttfa_ms": rec.e2e_ttfa_ms,
            "e2e_total_ms": rec.e2e_total_ms,
        }
        try:
            conn = await db_module.get_connection()
            try:
                await db_module.insert_conversation(conn, data)
            finally:
                await conn.close()
        except Exception as e:
            logger.error(f"[Main] 写入对话记录失败: {e}")
        # 重置 record 状态以备下次请求
        rec.stop_time = None
        rec.asr_start = rec.asr_first = rec.asr_end = None
        rec.llm_ttft = rec.llm_end = None
        rec.tts_ttfa = rec.tts_end = None
        rec.user_text = ""
        rec.ai_text = ""

    record.on_complete = _on_complete

    # STT：阿里云 Paraformer
    stt = DashScopeSTTService(
        api_key=Config.DASHSCOPE_API_KEY,
        model=Config.STT_MODEL,
        audio_passthrough=False,  # 不将音频帧传递到下游
        record=record,
    )

    # TTS：阿里云 CosyVoice
    tts = DashScopeTTSService(
        api_key=Config.DASHSCOPE_API_KEY,
        model=Config.TTS_MODEL,
        voice=Config.TTS_VOICE,
    )

    # LLM 上下文（含系统提示）
    messages = [{"role": "system", "content": Config.LLM_SYSTEM_PROMPT}]
    context = LLMContext(messages)
    context_aggregator = LLMContextAggregatorPair(context)

    # Ping 处理器
    ping_handler = PingHandler()

    # 延迟跟踪器（拦截 VADUserStoppedSpeakingFrame 记录 stop_time）
    latency_tracker = LatencyTracker(record=record)

    # STT 转录转发器（必须在 context_aggregator.user() 之前）
    transcript_forwarder = TranscriptForwarder(record=record)

    # LLM 文本捕获器（必须在 tts 之前，TTS 会消费 TextFrame 不再下传）
    llm_text_capture = LLMTextCapture(record=record)

    # TTS 帧转发器（MP3 + tts_start/end）
    tts_forwarder = TTSAudioForwarder(record=record)

    # Pipecat 管道
    pipeline = Pipeline([
        transport.input(),            # WebSocket 输入 → 反序列化为 Pipecat 帧
                                      #   binary → InputAudioRawFrame
                                      #   "start" → VADUserStartedSpeakingFrame
                                      #   "stop"  → VADUserStoppedSpeakingFrame
        ping_handler,                 # iOSPingFrame → pong，其余帧透传
        latency_tracker,              # 拦截 VADUserStoppedSpeakingFrame → stop_time
        stt,                          # STT：InputAudioRawFrame 累积 → TranscriptionFrame
        transcript_forwarder,         # TranscriptionFrame → transcript_final 发给 iOS，同时继续下传
        context_aggregator.user(),    # 将识别文本加入对话上下文（消费 TranscriptionFrame）
        llm,                          # LLM：生成回复（TextFrame × N + LLMFullResponseEndFrame）
        llm_text_capture,             # 拦截 TextFrame → llm_ttft/ai_text/llm_done（TTS 消费前）
        tts,                          # TTS：文字 → 音频（MP3）
        tts_forwarder,                # tts_start/audio/end → OutputTransportMessageUrgentFrame
        transport.output(),           # 序列化 → WebSocket 输出
        context_aggregator.assistant(),  # 将 AI 回复加入对话历史
    ])

    task = PipelineTask(pipeline, params=PipelineParams(allow_interruptions=False), enable_rtvi=False)

    @transport.event_handler("on_client_connected")
    async def on_connected(transport, client):
        logger.info("[Pipecat] iOS 客户端已连接")
        # 直接发送 ready 事件（StartFrame 不经过序列化器）
        await client.send_text(json.dumps({"type": "ready"}))
        await task.queue_frames([StartFrame()])

    @transport.event_handler("on_client_disconnected")
    async def on_disconnected(transport, client):
        logger.info("[Pipecat] iOS 客户端已断开")
        await task.cancel()

    runner = PipelineRunner(handle_sigint=False)
    await runner.run(task)


if __name__ == "__main__":
    uvicorn.run(
        "main:app",
        host=Config.SERVER_HOST,
        port=Config.SERVER_PORT,
        log_level="info",
    )
