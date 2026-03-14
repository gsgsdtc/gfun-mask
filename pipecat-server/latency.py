"""
@doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.2, §4.1
@purpose 语音管道计时系统：LatencyRecord（共享计时数据）+ LatencyTracker（FrameProcessor）
"""

from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from typing import Awaitable, Callable, Optional

from loguru import logger
from pipecat.frames.frames import Frame, VADUserStoppedSpeakingFrame
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

# 慢请求告警阈值（ms）
SLOW_THRESHOLD_MS = 1000


def _ms(start: Optional[float], end: Optional[float]) -> Optional[int]:
    """将两个 monotonic 时间戳转换为毫秒差值，任一为 None 则返回 None。"""
    if start is None or end is None:
        return None
    return round((end - start) * 1000)


@dataclass
class LatencyRecord:
    """
    一次语音请求的计时数据，由各 Processor 共享写入。
    所有时间戳使用 time.monotonic()（单调时钟，不受系统时间影响）。
    派生字段（*_ms）由 property 动态计算，无需手动维护。
    """

    session_id: str = field(default_factory=lambda: str(uuid.uuid4()))

    # 各阶段时间戳（由对应 Processor 赋值）
    stop_time: Optional[float] = None   # VADUserStoppedSpeakingFrame 到达时刻

    asr_start: Optional[float] = None   # run_stt() 进入
    asr_first: Optional[float] = None   # 批量模式 = asr_end；流式后为真正首包
    asr_end: Optional[float] = None     # TranscriptionFrame yield 前

    llm_ttft: Optional[float] = None    # 第一个 TextFrame 到达 TTSAudioForwarder
    llm_end: Optional[float] = None     # LLMFullResponseEndFrame 到达

    tts_ttfa: Optional[float] = None    # 第一个 TTSAudioRawFrame 到达 TTSAudioForwarder
    tts_end: Optional[float] = None     # TTSStoppedFrame 到达

    # 文本内容（由 TranscriptForwarder / TTSAudioForwarder 填充）
    user_text: str = ""
    ai_text: str = ""

    # 完成回调（tts_ttfa 触发，写 DB + 输出日志）
    on_complete: Optional[Callable[["LatencyRecord"], Awaitable[None]]] = None

    # ── 派生指标（ms） ──────────────────────────────────────────

    @property
    def asr_ttfa_ms(self) -> Optional[int]:
        return _ms(self.asr_start, self.asr_first)

    @property
    def asr_total_ms(self) -> Optional[int]:
        return _ms(self.asr_start, self.asr_end)

    @property
    def llm_ttft_ms(self) -> Optional[int]:
        return _ms(self.asr_end, self.llm_ttft)

    @property
    def llm_total_ms(self) -> Optional[int]:
        return _ms(self.asr_end, self.llm_end)

    @property
    def tts_ttfa_ms(self) -> Optional[int]:
        return _ms(self.llm_ttft, self.tts_ttfa)

    @property
    def tts_total_ms(self) -> Optional[int]:
        return _ms(self.llm_ttft, self.tts_end)

    @property
    def e2e_ttfa_ms(self) -> Optional[int]:
        return _ms(self.stop_time, self.tts_ttfa)

    @property
    def e2e_total_ms(self) -> Optional[int]:
        return _ms(self.stop_time, self.tts_end)

    # ── 日志摘要 ────────────────────────────────────────────────

    def log_summary(self) -> str:
        sid = self.session_id[:8] if self.session_id else "?"

        def fmt(v: Optional[int]) -> str:
            return f"{v}ms" if v is not None else "N/A"

        return (
            f"[Latency] session={sid} | "
            f"ASR_ttfa={fmt(self.asr_ttfa_ms)} | ASR={fmt(self.asr_total_ms)} | "
            f"LLM_TTFT={fmt(self.llm_ttft_ms)} | LLM={fmt(self.llm_total_ms)} | "
            f"TTS_ttfa={fmt(self.tts_ttfa_ms)} | TTS={fmt(self.tts_total_ms)} | "
            f"E2E_ttfa={fmt(self.e2e_ttfa_ms)} | E2E_total={fmt(self.e2e_total_ms)}"
        )

    def emit_log(self) -> None:
        """输出延迟摘要日志，慢请求额外输出 WARNING。"""
        logger.info(self.log_summary())
        slow_fields = {
            "ASR": self.asr_total_ms,
            "LLM_TTFT": self.llm_ttft_ms,
            "TTS_ttfa": self.tts_ttfa_ms,
            "E2E": self.e2e_ttfa_ms,
        }
        for name, val in slow_fields.items():
            if val is not None and val > SLOW_THRESHOLD_MS:
                logger.warning(f"[Latency] ⚠ 慢请求 {name}={val}ms (>{SLOW_THRESHOLD_MS}ms)")


class LatencyTracker(FrameProcessor):
    """
    管道中的纯旁路计时处理器。
    拦截 VADUserStoppedSpeakingFrame 记录 stop_time，其余帧完全透传。
    """

    def __init__(self, record: LatencyRecord, **kwargs):
        super().__init__(**kwargs)
        self._record = record

    async def process_frame(self, frame: Frame, direction: FrameDirection) -> None:
        await super().process_frame(frame, direction)
        if isinstance(frame, VADUserStoppedSpeakingFrame):
            self._record.stop_time = time.monotonic()
            logger.debug(f"[LatencyTracker] stop_time 已记录 session={self._record.session_id[:8]}")
        await self.push_frame(frame, direction)
