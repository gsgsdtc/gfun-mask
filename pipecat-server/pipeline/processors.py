"""
@doc     docs/modules/pipecat-pipeline/design/05-pipecat-server-refactor-backend-design.md §4.2
@purpose 管道 FrameProcessor 子类（从 main.py 提取）
"""

from __future__ import annotations

import json
import time

from loguru import logger
from pipecat.frames.frames import (
    ErrorFrame,
    Frame,
    LLMFullResponseEndFrame,
    OutputTransportMessageUrgentFrame,
    TTSAudioRawFrame,
    TTSStartedFrame,
    TTSStoppedFrame,
    TranscriptionFrame,
    TextFrame,
)
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

from core.latency import LatencyRecord
from pipeline.builder import TTS_AUDIO_PREFIX, iOSPingFrame


class PingHandler(FrameProcessor):
    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)
        if isinstance(frame, iOSPingFrame):
            logger.debug("[PingHandler] ping → pong")
            pong = OutputTransportMessageUrgentFrame(message=json.dumps({"type": "pong"}))
            await self.push_frame(pong)
        else:
            await self.push_frame(frame, direction)


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
            await self.push_frame(frame, direction)
        elif isinstance(frame, ErrorFrame):
            msg = json.dumps({"type": "error", "code": "STT_EMPTY", "message": str(frame.error)})
            logger.warning(f"[TranscriptForwarder] → error: {frame.error}")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=msg))
            await self.push_frame(frame, direction)
        else:
            await self.push_frame(frame, direction)


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
            self._tts_audio_received = False
            logger.info("[TTSForwarder] → tts_end")
            await self.push_frame(OutputTransportMessageUrgentFrame(message=json.dumps({"type": "tts_end"})))
            if self._record.on_complete:
                await self._record.on_complete(self._record)
        else:
            await self.push_frame(frame, direction)
