"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md
@purpose 基于 Pipecat 框架的语音聊天服务
         使用 FastAPIWebsocketTransport + 自定义 iOS 协议序列化器
         STT: DashScope Paraformer，LLM: Qwen，TTS: DashScope CosyVoice
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field

import uvicorn
from fastapi import FastAPI
from fastapi.websockets import WebSocket

from pipecat.frames.frames import (
    AudioRawFrame,
    EndFrame,
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
from loguru import logger

from config import Config, DASHSCOPE_BASE_URL
from dashscope_services import DashScopeSTTService, DashScopeTTSService

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
    - 序列化：Pipecat Frame → iOS JSON/binary
    """

    async def serialize(self, frame: Frame) -> str | bytes | None:
        """将 Pipecat 内部帧转为 iOS 期望的 WebSocket 消息格式"""
        if isinstance(frame, TranscriptionFrame):
            msg = json.dumps({"type": "transcript_final", "text": frame.text})
            logger.info(f"[Serializer] → transcript_final: '{frame.text}'")
            return msg
        elif isinstance(frame, TextFrame):
            msg = json.dumps({"type": "llm_done", "text": frame.text})
            logger.info(f"[Serializer] → llm_done: '{frame.text[:60]}...' ({len(frame.text)} chars)")
            return msg
        elif isinstance(frame, TTSStartedFrame):
            logger.info("[Serializer] → tts_start")
            return json.dumps({"type": "tts_start"})
        elif isinstance(frame, TTSAudioRawFrame):
            # TTSAudioForwarder 已将 TTSAudioRawFrame 转为 OutputTransportMessageUrgentFrame
            # 此处不应再收到 TTSAudioRawFrame，但保留作为后备
            data = bytes([TTS_AUDIO_PREFIX]) + frame.audio
            logger.debug(f"[Serializer] → tts_audio chunk (fallback): {len(frame.audio)} bytes")
            return data
        elif isinstance(frame, TTSStoppedFrame):
            logger.info("[Serializer] → tts_end")
            return json.dumps({"type": "tts_end"})
        elif isinstance(frame, OutputTransportMessageUrgentFrame):
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
            logger.info("[Serializer] ← start → UserStartedSpeakingFrame")
            return UserStartedSpeakingFrame()
        elif msg_type == "stop":
            logger.info("[Serializer] ← stop → UserStoppedSpeakingFrame")
            return UserStoppedSpeakingFrame()
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
# TTS 音频转发器
# TTSAudioRawFrame 含 MP3 数据，直接进 transport 会触发 PCM resampler 报错
# 在此提前转换为 OutputTransportMessageUrgentFrame（binary），绕过 resampler
# ──────────────────────────────────────────────

class TTSAudioForwarder(FrameProcessor):
    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)
        if isinstance(frame, TTSAudioRawFrame):
            data = bytes([TTS_AUDIO_PREFIX]) + frame.audio
            logger.debug(f"[TTSForwarder] → tts_audio binary: {len(frame.audio)} bytes MP3")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=data))
        else:
            await self.push_frame(frame, direction)


# ──────────────────────────────────────────────
# FastAPI 应用
# ──────────────────────────────────────────────

app = FastAPI(title="VoiceMask Pipecat Server")


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    transport = FastAPIWebsocketTransport(
        websocket=websocket,
        params=FastAPIWebsocketParams(
            audio_in_enabled=False,   # 序列化器直接生成 InputAudioRawFrame，无需 transport 处理音频输入
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

    # STT：阿里云 Paraformer
    stt = DashScopeSTTService(
        api_key=Config.DASHSCOPE_API_KEY,
        model=Config.STT_MODEL,
        audio_passthrough=False,  # 不将音频帧传递到下游
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

    # TTS 音频转发器（MP3 → binary frame，绕过 PCM resampler）
    tts_forwarder = TTSAudioForwarder()

    # Pipecat 管道（原生音频处理）
    pipeline = Pipeline([
        transport.input(),         # WebSocket 输入 → 反序列化为 Pipecat 帧
                                   #   binary → InputAudioRawFrame
                                   #   "start" → UserStartedSpeakingFrame
                                   #   "stop"  → UserStoppedSpeakingFrame
        ping_handler,              # iOSPingFrame → pong，其余帧透传
        stt,                       # STT 原生接收 InputAudioRawFrame，在 UserStarted/Stopped 间累积
        context_aggregator.user(), # 将识别文本加入对话上下文
        llm,                       # LLM：生成回复
        tts,                       # TTS：文字 → 音频（MP3）
        tts_forwarder,             # TTSAudioRawFrame(MP3) → OutputTransportMessageUrgentFrame
        transport.output(),        # 序列化 → WebSocket 输出
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
