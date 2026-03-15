"""
@doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.3
@purpose 验证 TTSAudioForwarder 多句子生命周期状态机正确性
@context TTSAudioForwarder 负责将 TTS 事件转发给 iOS 并触发 on_complete 回调；
         Pipecat 已内置句子级 TTS 聚合，多句回复会产生多个 TTSStarted/TTSStopped 对；
         若 on_complete 在第一句结束时即触发，数据库写入会在回复未完成时执行（数据截断）。
@depends pipeline.processors.TTSAudioForwarder, core.latency.LatencyRecord
"""

import pytest
import asyncio
from unittest.mock import AsyncMock, MagicMock, patch
from pipecat.frames.frames import (
    TTSAudioRawFrame,
    TTSStartedFrame,
    TTSStoppedFrame,
    LLMFullResponseEndFrame,
)
from pipecat.processors.frame_processor import FrameDirection

from core.latency import LatencyRecord


def _make_forwarder():
    from pipeline.processors import TTSAudioForwarder
    record = LatencyRecord(session_id="test")
    record.on_complete = AsyncMock()
    forwarder = TTSAudioForwarder(record=record)
    forwarder.push_frame = AsyncMock()
    return forwarder, record


# ─── 单句场景（向后兼容） ──────────────────────────────────────────────────────

class TestTTSAudioForwarderSingleSentence:

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.3
    # @purpose 验证单句回复场景下 on_complete 在 TTSStopped + LLM done 后触发一次
    # @context 单句是最常见的当前场景（≤50字）；若此场景下 on_complete 不触发，数据库写入中断
    @pytest.mark.asyncio
    async def test_single_sentence_on_complete_fires_once(self):
        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM

        # LLM done → then TTS cycle
        await forwarder.process_frame(LLMFullResponseEndFrame(), direction)
        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        await forwarder.process_frame(TTSStoppedFrame(), direction)

        record.on_complete.assert_called_once_with(record)

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.3
    # @purpose 验证单句场景下 tts_ttfa 被正确记录（只记第一帧）
    # @context tts_ttfa 是 E2E 首包时间的最后分量；未记录会导致 e2e_ttfa_ms 计算返回 None
    @pytest.mark.asyncio
    async def test_single_sentence_tts_ttfa_recorded(self):
        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM

        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        await forwarder.process_frame(TTSStoppedFrame(), direction)

        assert record.tts_ttfa is not None


# ─── 多句场景（流水线支持） ────────────────────────────────────────────────────

class TestTTSAudioForwarderMultiSentence:

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.3
    # @purpose 验证多句回复时 on_complete 只在最后一句结束后触发一次
    # @context Pipecat 内置句子聚合后，多句回复会产生多个 TTSStarted/TTSStopped 对；
    #          若 on_complete 在第一个 TTSStopped 时触发，数据库写入会在回复未完成时执行，
    #          导致 ai_text 和 tts_end 数据不完整
    @pytest.mark.asyncio
    async def test_multi_sentence_on_complete_fires_once_at_end(self):
        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM

        # 句子1
        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        await forwarder.process_frame(TTSStoppedFrame(), direction)

        # on_complete 不应在此触发（LLM 尚未完成）
        record.on_complete.assert_not_called()

        # 句子2
        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        await forwarder.process_frame(TTSStoppedFrame(), direction)

        # LLM done 后，最后一个 TTSStopped 触发 on_complete
        await forwarder.process_frame(LLMFullResponseEndFrame(), direction)

        record.on_complete.assert_called_once_with(record)

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.3
    # @purpose 验证多句场景下 tts_ttfa 只记录第一句第一帧，不被后续句子覆盖
    # @context tts_ttfa 语义是"LLM 首 Token 到第一帧 TTS 音频的时间"；
    #          若被第二句音频时间覆盖，tts_ttfa_ms 和 e2e_ttfa_ms 将偏大，数据失真
    @pytest.mark.asyncio
    async def test_multi_sentence_tts_ttfa_only_first(self):
        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM

        # 句子1 音频
        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        first_ttfa = record.tts_ttfa
        assert first_ttfa is not None

        await forwarder.process_frame(TTSStoppedFrame(), direction)

        # 句子2 音频
        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )

        # tts_ttfa 不应被覆盖
        assert record.tts_ttfa == first_ttfa

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.3
    # @purpose 验证多句场景下 tts_end 为最后一句的结束时间
    # @context tts_total_ms = llm_ttft → tts_end；若 tts_end 停在第一句，tts_total 将严重偏小
    @pytest.mark.asyncio
    async def test_multi_sentence_tts_end_is_last(self):
        import time
        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM

        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        await forwarder.process_frame(TTSStoppedFrame(), direction)
        tts_end_after_s1 = record.tts_end

        # 短暂延迟后句子2
        await asyncio.sleep(0.01)

        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        await forwarder.process_frame(TTSStoppedFrame(), direction)

        assert record.tts_end > tts_end_after_s1

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.3
    # @purpose 验证 iOS 协议中 tts_start 只发送一次（对 iOS 端透明）
    # @context iOS 客户端依赖 tts_start/tts_end 更新播放状态；多次 tts_start 会导致 iOS 状态混乱
    @pytest.mark.asyncio
    async def test_multi_sentence_tts_start_sent_once_to_ios(self):
        from pipecat.frames.frames import OutputTransportMessageUrgentFrame
        import json

        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM
        sent_messages = []

        async def capture_push(frame, direction=None):
            if isinstance(frame, OutputTransportMessageUrgentFrame):
                if isinstance(frame.message, str):
                    sent_messages.append(json.loads(frame.message))

        forwarder.push_frame = capture_push

        # 两句 TTS
        for _ in range(2):
            await forwarder.process_frame(TTSStartedFrame(), direction)
            await forwarder.process_frame(
                TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
            )
            await forwarder.process_frame(TTSStoppedFrame(), direction)

        await forwarder.process_frame(LLMFullResponseEndFrame(), direction)

        tts_start_count = sum(1 for m in sent_messages if m.get("type") == "tts_start")
        tts_end_count = sum(1 for m in sent_messages if m.get("type") == "tts_end")

        assert tts_start_count == 1, f"期望 tts_start 发送一次，实际 {tts_start_count} 次"
        assert tts_end_count == 1, f"期望 tts_end 发送一次，实际 {tts_end_count} 次"


# ─── 边界场景 ──────────────────────────────────────────────────────────────────

class TestTTSAudioForwarderEdgeCases:

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.6
    # @purpose 验证 LLM 输出空回复时（无 TTS），on_complete 仍正常触发
    # @context 空回复场景：LLM 返回空文本，Pipecat 不调用 TTS；
    #          若 on_complete 依赖 TTSStopped，空回复时将永不触发，导致数据库漏写
    @pytest.mark.asyncio
    async def test_empty_reply_no_tts_on_complete_still_fires(self):
        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM

        # 无 TTS 调用，直接 LLMFullResponseEndFrame
        await forwarder.process_frame(LLMFullResponseEndFrame(), direction)

        record.on_complete.assert_called_once_with(record)

    # @doc     docs/modules/pipecat-pipeline/design/06-pipeline-latency-optimization-backend-design.md §4.6
    # @purpose 验证多轮对话时状态机在每轮结束后正确重置
    # @context 若状态未重置（如 _llm_done 残留），下一轮首个 TTSStopped 会意外触发 on_complete
    @pytest.mark.asyncio
    async def test_state_resets_between_turns(self):
        forwarder, record = _make_forwarder()
        direction = FrameDirection.DOWNSTREAM

        # 第一轮
        await forwarder.process_frame(LLMFullResponseEndFrame(), direction)
        await forwarder.process_frame(TTSStartedFrame(), direction)
        await forwarder.process_frame(
            TTSAudioRawFrame(audio=b"\x00" * 100, sample_rate=22050, num_channels=1), direction
        )
        await forwarder.process_frame(TTSStoppedFrame(), direction)

        assert record.on_complete.call_count == 1

        # 第二轮开始，不应立即触发
        await forwarder.process_frame(TTSStartedFrame(), direction)
        assert record.on_complete.call_count == 1  # 仍是 1，未提前触发
