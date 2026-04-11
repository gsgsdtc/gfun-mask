"""
@doc     docs/modules/pipecat-pipeline/design/05-pipecat-server-refactor-backend-design.md §4.2
@purpose iOS 协议序列化器、iOSPingFrame、build_pipeline 工厂函数
"""

from __future__ import annotations

import json
import time
from enum import Enum
from typing import Callable, Awaitable, Optional

from fastapi.websockets import WebSocket
from loguru import logger
from pipecat.frames.frames import (
    Frame,
    InputAudioRawFrame,
    LLMFullResponseEndFrame,
    OutputTransportMessageUrgentFrame,
    StartFrame,
    VADUserStartedSpeakingFrame,
    VADUserStoppedSpeakingFrame,
)
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import LLMContextAggregatorPair
from pipecat.serializers.base_serializer import FrameSerializer
from pipecat.services.qwen.llm import QwenLLMService
from pipecat.services.openai.llm import OpenAILLMService
from pipecat.transports.websocket.fastapi import (
    FastAPIWebsocketParams,
    FastAPIWebsocketTransport,
)

from config import Config, DASHSCOPE_BASE_URL, LMSTUDIO_BASE_URL
from core.latency import LatencyRecord, LatencyTracker
from services.dashscope import DashScopeSTTService, DashScopeTTSService

TTS_AUDIO_PREFIX = 0xAA


class iOSPingFrame(Frame):
    pass


# feat-08: 新增控制帧类型
class iOSInterruptFrame(Frame):
    """用户打断 TTS 播放"""
    pass


class iOSWakeFrame(Frame):
    """设备从休眠唤醒"""
    pass


class iOSSleepFrame(Frame):
    """设备进入休眠"""
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
        # feat-08: 新增消息类型处理
        elif msg_type == "interrupt":
            logger.info("[Serializer] ← interrupt → iOSInterruptFrame")
            return iOSInterruptFrame()
        elif msg_type == "wake":
            logger.info("[Serializer] ← wake → iOSWakeFrame")
            return iOSWakeFrame()
        elif msg_type == "sleep":
            logger.info("[Serializer] ← sleep → iOSSleepFrame")
            return iOSSleepFrame()
        return None


# feat-08: Session 状态管理
class SessionState(Enum):
    LISTENING = "listening"       # 监听中，等待用户说话
    PROCESSING = "processing"     # 处理中，ASR -> LLM -> TTS
    TTS_PLAYING = "tts_playing"   # TTS 正在播放
    SLEEP = "sleep"               # 休眠状态


class SessionManager(FrameProcessor):
    """
    feat-08: Session 状态管理器
    处理 interrupt/wake/sleep 消息，管理状态转换
    """

    def __init__(self, record: LatencyRecord, transport, **kwargs):
        super().__init__(**kwargs)
        self._record = record
        self._transport = transport
        self._state = SessionState.LISTENING
        self._task: Optional[PipelineTask] = None

    def set_task(self, task: PipelineTask):
        self._task = task

    @property
    def state(self) -> SessionState:
        return self._state

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)

        # 状态转换检测
        if isinstance(frame, VADUserStoppedSpeakingFrame):
            self._state = SessionState.PROCESSING
            logger.info(f"[Session] State → PROCESSING")

        elif isinstance(frame, LLMFullResponseEndFrame):
            if self._state == SessionState.PROCESSING:
                # TTS 即将开始
                pass

        # 处理控制帧
        if isinstance(frame, iOSInterruptFrame):
            await self._handle_interrupt()
        elif isinstance(frame, iOSWakeFrame):
            await self._handle_wake()
        elif isinstance(frame, iOSSleepFrame):
            await self._handle_sleep()

        await self.push_frame(frame, direction)

    async def _send_text_to_client(self, text: str):
        """通过 transport 发送文本消息到客户端"""
        try:
            # 使用 transport 的 websocket 发送
            websocket = getattr(self._transport, '_websocket', None)
            if websocket:
                await websocket.send_text(text)
        except Exception as e:
            logger.warning(f"[Session] 发送消息失败: {e}")

    async def _handle_interrupt(self):
        """处理打断：取消当前任务，清空 TTS 队列"""
        self._record.interrupt_at = time.monotonic()
        self._record.interrupt_count += 1
        logger.info(f"[Session] Interrupt received, count={self._record.interrupt_count}")

        # 取消当前管道任务
        if self._task and self._state in (SessionState.PROCESSING, SessionState.TTS_PLAYING):
            logger.info("[Session] Cancelling current pipeline task")
            try:
                await self._task.cancel()
                # 通知 iOS 打断完成
                await self._send_text_to_client(json.dumps({"type": "interrupt_ack"}))
            except Exception as e:
                logger.error(f"[Session] 打断任务失败: {e}")

        self._state = SessionState.LISTENING

    async def _handle_wake(self):
        """处理唤醒：重置状态"""
        self._record.wake_at = time.monotonic()
        self._record.wake_count += 1
        self._state = SessionState.LISTENING
        logger.info(f"[Session] Wake received, count={self._record.wake_count}, state → LISTENING")

    async def _handle_sleep(self):
        """处理休眠：记录状态"""
        self._state = SessionState.SLEEP
        logger.info("[Session] Sleep received, state → SLEEP")


async def build_pipeline(
    websocket: WebSocket,
    record: LatencyRecord,
    on_complete: Callable[[LatencyRecord], Awaitable[None]],
) -> PipelineTask:
    """
    组装 Pipecat 管道并返回 PipelineTask。
    主调方（websocket_endpoint）负责 runner.run(task)。
    """
    from pipecat.frames.frames import LLMFullResponseEndFrame
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

    # feat-08: Session 管理器（需要在其他处理器之前）
    session_mgr = SessionManager(record=record, transport=transport)

    # 根据配置选择 LLM 提供商
    if Config.LLM_PROVIDER == "lmstudio":
        logger.info(f"[Pipeline] 使用 LM Studio 本地模型: {Config.LMSTUDIO_MODEL} @ {LMSTUDIO_BASE_URL}")
        # 禁用 Qwen3.5 thinking 模式的额外参数
        extra_params = {}
        if "qwen" in Config.LLMSTUDIO_MODEL.lower():
            extra_params = {"enable_thinking": False}
            logger.info("[Pipeline] 已禁用 Qwen thinking 模式")
        llm = OpenAILLMService(
            api_key=Config.LMSTUDIO_API_KEY,
            base_url=LMSTUDIO_BASE_URL,
            model=Config.LMSTUDIO_MODEL,
            temperature=0.7,
            max_tokens=100,  # 限制输出长度，加快响应
            stream=True,     # 强制启用流式模式
            params=extra_params if extra_params else None,
        )
    else:
        logger.info(f"[Pipeline] 使用 DashScope 通义千问: {Config.LLM_MODEL}")
        llm = QwenLLMService(
            api_key=Config.DASHSCOPE_API_KEY,
            base_url=DASHSCOPE_BASE_URL,
            model=Config.LLM_MODEL,
            temperature=0.7,
            max_tokens=100,
            stream=True,
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
        session_mgr,  # feat-08: Session 管理器
        LatencyTracker(record=record),
        stt,
        TranscriptForwarder(record=record),
        context_aggregator.user(),
        llm,
        LLMTextCapture(record=record),
        tts,
        TTSAudioForwarder(record=record, session_mgr=session_mgr),
        transport.output(),
        context_aggregator.assistant(),
    ])

    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            allow_interruptions=True,  # 允许打断，提升响应性
            enable_metrics=True,
            enable_usage_metrics=True,
        ),
        enable_rtvi=False,
    )

    # feat-08: 设置 task 引用到 session manager
    session_mgr.set_task(task)

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
            "interrupt_count": rec.interrupt_count,
            "wake_count": rec.wake_count,
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
