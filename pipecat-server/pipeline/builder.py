"""
@doc     docs/modules/pipecat-pipeline/design/05-pipecat-server-refactor-backend-design.md §4.2
@purpose iOS 协议序列化器、iOSPingFrame、build_pipeline 工厂函数
"""

from __future__ import annotations

import json
from typing import Callable, Awaitable

from fastapi.websockets import WebSocket
from loguru import logger
from pipecat.frames.frames import (
    Frame,
    InputAudioRawFrame,
    OutputTransportMessageUrgentFrame,
    StartFrame,
    VADUserStartedSpeakingFrame,
    VADUserStoppedSpeakingFrame,
)
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import LLMContextAggregatorPair
from pipecat.serializers.base_serializer import FrameSerializer
from pipecat.services.qwen.llm import QwenLLMService
from pipecat.transports.websocket.fastapi import (
    FastAPIWebsocketParams,
    FastAPIWebsocketTransport,
)

from config import Config, DASHSCOPE_BASE_URL
from core.latency import LatencyRecord, LatencyTracker
from services.dashscope import DashScopeSTTService, DashScopeTTSService

TTS_AUDIO_PREFIX = 0xAA


class iOSPingFrame(Frame):
    pass


class iOSProtocolSerializer(FrameSerializer):
    """
    iOS ↔ Pipecat 协议转换：
    - 反序列化：iOS JSON/binary → Pipecat Frame
    - 序列化：只处理 OutputTransportMessageUrgentFrame
    """

    async def serialize(self, frame: Frame) -> str | bytes | None:
        if isinstance(frame, OutputTransportMessageUrgentFrame):
            logger.debug(f"[Serializer] → urgent: {type(frame.message).__name__} {len(frame.message) if isinstance(frame.message, (bytes, str)) else ''}")
            return frame.message
        return None

    async def deserialize(self, data: str | bytes) -> Frame | None:
        if isinstance(data, bytes):
            return InputAudioRawFrame(audio=data, sample_rate=16000, num_channels=1)

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


async def build_pipeline(
    websocket: WebSocket,
    record: LatencyRecord,
    on_complete: Callable[[LatencyRecord], Awaitable[None]],
) -> PipelineTask:
    """
    组装 Pipecat 管道并返回 PipelineTask。
    主调方（websocket_endpoint）负责 runner.run(task)。
    """
    from pipeline.processors import (
        PingHandler,
        TranscriptForwarder,
        LLMTextCapture,
        TTSAudioForwarder,
    )

    transport = FastAPIWebsocketTransport(
        websocket=websocket,
        params=FastAPIWebsocketParams(
            audio_in_enabled=True,
            audio_out_enabled=True,
            serializer=iOSProtocolSerializer(),
        ),
    )

    llm = QwenLLMService(
        api_key=Config.DASHSCOPE_API_KEY,
        base_url=DASHSCOPE_BASE_URL,
        model=Config.LLM_MODEL,
    )

    stt = DashScopeSTTService(
        api_key=Config.DASHSCOPE_API_KEY,
        model=Config.STT_MODEL,
        audio_passthrough=False,
        record=record,
    )

    tts = DashScopeTTSService(
        api_key=Config.DASHSCOPE_API_KEY,
        model=Config.TTS_MODEL,
        voice=Config.TTS_VOICE,
    )

    messages = [{"role": "system", "content": Config.LLM_SYSTEM_PROMPT}]
    context = LLMContext(messages)
    context_aggregator = LLMContextAggregatorPair(context)

    record.on_complete = on_complete

    pipeline = Pipeline([
        transport.input(),
        PingHandler(),
        LatencyTracker(record=record),
        stt,
        TranscriptForwarder(record=record),
        context_aggregator.user(),
        llm,
        LLMTextCapture(record=record),
        tts,
        TTSAudioForwarder(record=record),
        transport.output(),
        context_aggregator.assistant(),
    ])

    task = PipelineTask(pipeline, params=PipelineParams(allow_interruptions=False), enable_rtvi=False)

    @transport.event_handler("on_client_connected")
    async def on_connected(transport, client):
        logger.info("[Pipecat] iOS 客户端已连接")
        await client.send_text(json.dumps({"type": "ready"}))
        await task.queue_frames([StartFrame()])

    @transport.event_handler("on_client_disconnected")
    async def on_disconnected(transport, client):
        logger.info("[Pipecat] iOS 客户端已断开")
        await task.cancel()

    return task


def make_on_complete(db_mod) -> Callable[[LatencyRecord], Awaitable[None]]:
    """
    工厂函数：返回一个 on_complete 回调，负责写 DB 并重置 LatencyRecord。
    db_mod 为 core.db 模块，避免循环导入。
    """
    from datetime import datetime, timezone
    from loguru import logger as _logger

    async def _on_complete(rec: LatencyRecord) -> None:
        rec.emit_log()
        data = {
            "session_id": rec.session_id,
            "created_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "user_text": rec.user_text,
            "ai_text": rec.ai_text,
            "asr_ttfa_ms": rec.asr_ttfa_ms,  "asr_total_ms": rec.asr_total_ms,
            "llm_ttft_ms": rec.llm_ttft_ms,  "llm_total_ms": rec.llm_total_ms,
            "tts_ttfa_ms": rec.tts_ttfa_ms,  "tts_total_ms": rec.tts_total_ms,
            "e2e_ttfa_ms": rec.e2e_ttfa_ms,  "e2e_total_ms": rec.e2e_total_ms,
        }
        try:
            conn = await db_mod.get_connection()
            try:
                await db_mod.insert_conversation(conn, data)
            finally:
                await conn.close()
        except Exception as e:
            _logger.error(f"[Main] 写入对话记录失败: {e}")
        rec.stop_time = None
        rec.asr_start = rec.asr_first = rec.asr_end = None
        rec.llm_ttft = rec.llm_end = None
        rec.tts_ttfa = rec.tts_end = None
        rec.user_text = rec.ai_text = ""

    return _on_complete
