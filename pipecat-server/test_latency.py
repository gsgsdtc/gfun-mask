"""
@doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md
@purpose 验证 LatencyRecord 计时字段和派生指标计算，以及 LatencyTracker 拦截行为
@context latency.py 是计时系统的核心；如果派生指标计算错误，
         日志和数据库中记录的延迟数据将不可信，无法指导性能优化。
@depends latency.LatencyRecord, latency.LatencyTracker
"""

import time
import pytest
import asyncio
from unittest.mock import AsyncMock, MagicMock


class TestLatencyRecordComputed:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.2
    # @purpose 验证 LatencyRecord 能正确计算各环节 ms 派生字段
    # @context 若 to_ms() 或派生计算错误，DB 中存储的耗时数据将失真，
    #          开发者无法判断真实瓶颈所在
    def test_asr_total_ms_computed(self):
        from latency import LatencyRecord
        r = LatencyRecord(session_id="test-1")
        t0 = time.monotonic()
        r.asr_start = t0
        r.asr_end = t0 + 0.34
        assert r.asr_total_ms == 340

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.2
    # @purpose 验证 asr_ttfa_ms（批量模式下等于 asr_total_ms）
    # @context 批量 ASR 无真正首包概念，首包=总耗时；
    #          字段保留为流式升级预留，当前必须与 total 相等
    def test_asr_ttfa_ms_equals_total_in_batch_mode(self):
        from latency import LatencyRecord
        r = LatencyRecord(session_id="test-2")
        t0 = time.monotonic()
        r.asr_start = t0
        r.asr_first = t0 + 0.34
        r.asr_end = t0 + 0.34
        assert r.asr_ttfa_ms == r.asr_total_ms

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.2
    # @purpose 验证 llm_ttft_ms 正确计算（从 STT 返回到首 Token）
    # @context LLM TTFT 是用户感知延迟的关键分量；若计算基准点错误会误导优化方向
    def test_llm_ttft_ms_computed(self):
        from latency import LatencyRecord
        r = LatencyRecord(session_id="test-3")
        t0 = time.monotonic()
        r.asr_end = t0
        r.llm_ttft = t0 + 0.49
        assert r.llm_ttft_ms == 490

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.2
    # @purpose 验证 e2e_ttfa_ms = tts_ttfa - stop_time
    # @context 整体首包时间是用户可感知延迟的核心指标；
    #          计算错误会导致优化目标（降低 e2e）无法正确衡量
    def test_e2e_ttfa_ms_computed(self):
        from latency import LatencyRecord
        r = LatencyRecord(session_id="test-4")
        t0 = time.monotonic()
        r.stop_time = t0
        r.tts_ttfa = t0 + 1.05
        assert r.e2e_ttfa_ms == 1050

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §2.2
    # @purpose 验证计时字段未设置时返回 None 而非抛出异常
    # @context 管道中间某环节失败（如 STT 空结果）时，部分字段可能未被赋值；
    #          此时应优雅返回 None 而非崩溃
    def test_missing_fields_return_none(self):
        from latency import LatencyRecord
        r = LatencyRecord(session_id="test-5")
        assert r.asr_total_ms is None
        assert r.llm_ttft_ms is None
        assert r.e2e_ttfa_ms is None

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §4.4
    # @purpose 验证 log_summary() 输出包含所有关键字段的字符串
    # @context 日志摘要是开发者实时监控的主要手段；
    #          格式缺少字段会导致无法快速定位慢请求的瓶颈环节
    def test_log_summary_contains_key_fields(self):
        from latency import LatencyRecord
        r = LatencyRecord(session_id="abcdefgh-1234")
        t0 = time.monotonic()
        r.stop_time = t0
        r.asr_start = t0
        r.asr_first = t0 + 0.34
        r.asr_end = t0 + 0.34
        r.llm_ttft = t0 + 0.83
        r.llm_end = t0 + 1.54
        r.tts_ttfa = t0 + 1.05
        r.tts_end = t0 + 1.94
        summary = r.log_summary()
        assert "ASR" in summary
        assert "LLM" in summary
        assert "TTS" in summary
        assert "E2E" in summary


class TestLatencyTracker:
    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §4.1
    # @purpose 验证 LatencyTracker 拦截 VADUserStoppedSpeakingFrame 并记录 stop_time
    # @context stop_time 是 e2e_ttfa 计算的起点；若未被捕获，整体首包时间将无法计算
    @pytest.mark.asyncio
    async def test_intercepts_vad_stopped_frame(self):
        from latency import LatencyRecord, LatencyTracker
        from pipecat.frames.frames import VADUserStoppedSpeakingFrame
        from pipecat.processors.frame_processor import FrameDirection

        record = LatencyRecord(session_id="tracker-test")
        tracker = LatencyTracker(record=record)
        tracker.push_frame = AsyncMock()

        frame = VADUserStoppedSpeakingFrame()
        await tracker.process_frame(frame, FrameDirection.DOWNSTREAM)

        assert record.stop_time is not None
        tracker.push_frame.assert_called_once_with(frame, FrameDirection.DOWNSTREAM)

    # @doc     docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md §4.1
    # @purpose 验证 LatencyTracker 对其他帧透传不干扰
    # @context LatencyTracker 是纯旁路观测，不能影响正常帧流；
    #          若误拦截其他帧会导致管道中断
    @pytest.mark.asyncio
    async def test_passes_through_other_frames(self):
        from latency import LatencyRecord, LatencyTracker
        from pipecat.frames.frames import VADUserStartedSpeakingFrame
        from pipecat.processors.frame_processor import FrameDirection

        record = LatencyRecord(session_id="tracker-test-2")
        tracker = LatencyTracker(record=record)
        tracker.push_frame = AsyncMock()

        frame = VADUserStartedSpeakingFrame()
        await tracker.process_frame(frame, FrameDirection.DOWNSTREAM)

        assert record.stop_time is None  # 未设置
        tracker.push_frame.assert_called_once_with(frame, FrameDirection.DOWNSTREAM)
