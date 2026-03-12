"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4
@purpose STT→LLM→TTS 管道编排，每个 WebSocket 连接共享一个管道实例
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import AsyncGenerator, Callable, List, Dict, Any, Awaitable


@dataclass
class PipelineEvent:
    """管道向外发出的事件，由 WebSocket handler 序列化后推送给 iOS。"""
    type: str
    data: Dict[str, Any] = field(default_factory=dict)
    audio: bytes = b""  # tts_audio 事件携带的音频数据


class VoicePipeline:
    """
    轻量管道：不直接依赖 pipecat 库，通过可注入的 async callable 实现 STT/LLM/TTS。
    生产环境可将 stt_fn/llm_fn/tts_fn 替换为真实 pipecat service 封装。
    """

    def __init__(
        self,
        stt_fn: Callable[[bytes], Awaitable[str]],
        llm_fn: Callable[[List[Dict]], Awaitable[str]],
        tts_fn: Callable[[str], Awaitable[bytes]],
    ):
        self._stt = stt_fn
        self._llm = llm_fn
        self._tts = tts_fn

    async def process_round(
        self,
        audio: bytes,
        history: List[Dict],
    ) -> AsyncGenerator[PipelineEvent, None]:
        """
        处理一轮对话：
          1. STT：audio → transcript
          2. LLM：messages → reply
          3. TTS：reply → audio_bytes
        每步结果以 PipelineEvent 形式 yield 给调用方。
        """
        # ── Step 1: STT ──
        try:
            transcript: str = await self._stt(audio)
        except Exception as exc:
            yield PipelineEvent(
                type="error",
                data={"code": "STT_FAIL", "message": str(exc)},
            )
            return

        yield PipelineEvent(
            type="transcript_final",
            data={"text": transcript},
        )

        # STT 为空时不调用 LLM（静音帧场景）
        if not transcript.strip():
            return

        # ── Step 2: LLM ──
        messages = list(history) + [{"role": "user", "content": transcript}]
        try:
            reply: str = await self._llm(messages)
        except Exception as exc:
            yield PipelineEvent(
                type="error",
                data={"code": "LLM_FAIL", "message": str(exc)},
            )
            return

        yield PipelineEvent(
            type="llm_done",
            data={"text": reply},
        )

        # ── Step 3: TTS ──
        yield PipelineEvent(type="tts_start")
        try:
            audio_bytes: bytes = await self._tts(reply)
        except Exception as exc:
            yield PipelineEvent(
                type="error",
                data={"code": "TTS_FAIL", "message": str(exc)},
            )
            return

        yield PipelineEvent(type="tts_audio", audio=audio_bytes)
        yield PipelineEvent(type="tts_end")
