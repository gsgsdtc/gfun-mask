"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4
@purpose 验证 Pipecat 管道处理器的核心逻辑（Mock STT/LLM/TTS）
@context pipeline.py 是 STT→LLM→TTS 的编排核心；
         如果管道逻辑错误（如跳过 STT、历史未传给 LLM），
         会导致 AI 无法理解用户、回复与上下文无关，整个语音聊天功能失效。
@depends session.Session, protocol.encode_json, protocol.encode_tts_audio
"""

import pytest
import asyncio
from unittest.mock import AsyncMock, MagicMock, patch
from pipeline.voice import VoicePipeline, PipelineEvent


class TestVoicePipelineEvents:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.2
    # @purpose 验证一个完整对话轮次产生正确的事件序列
    # @context VoicePipeline.process_round() 是单轮处理入口；
    #          事件序列错误（如缺少 transcript_final）会导致 iOS 界面无法更新对话气泡
    @pytest.mark.asyncio
    async def test_process_round_emits_transcript_event(self):
        mock_stt = AsyncMock(return_value="你好世界")
        mock_llm = AsyncMock(return_value="你好！")
        mock_tts = AsyncMock(return_value=bytes(1024))

        pipeline = VoicePipeline(stt_fn=mock_stt, llm_fn=mock_llm, tts_fn=mock_tts)
        events = []
        async for event in pipeline.process_round(audio=bytes(640), history=[]):
            events.append(event)

        types = [e.type for e in events]
        assert "transcript_final" in types

    @pytest.mark.asyncio
    async def test_process_round_emits_llm_done_event(self):
        pipeline = VoicePipeline(
            stt_fn=AsyncMock(return_value="问题"),
            llm_fn=AsyncMock(return_value="回答"),
            tts_fn=AsyncMock(return_value=bytes(512)),
        )
        events = []
        async for event in pipeline.process_round(audio=bytes(640), history=[]):
            events.append(event)

        types = [e.type for e in events]
        assert "llm_done" in types

    @pytest.mark.asyncio
    async def test_process_round_emits_tts_start_and_end(self):
        pipeline = VoicePipeline(
            stt_fn=AsyncMock(return_value="hi"),
            llm_fn=AsyncMock(return_value="hello"),
            tts_fn=AsyncMock(return_value=bytes(512)),
        )
        events = []
        async for event in pipeline.process_round(audio=bytes(640), history=[]):
            events.append(event)

        types = [e.type for e in events]
        assert "tts_start" in types
        assert "tts_audio" in types
        assert "tts_end" in types

    @pytest.mark.asyncio
    async def test_process_round_event_order(self):
        """事件顺序：transcript_final → llm_done → tts_start → tts_audio → tts_end"""
        pipeline = VoicePipeline(
            stt_fn=AsyncMock(return_value="问"),
            llm_fn=AsyncMock(return_value="答"),
            tts_fn=AsyncMock(return_value=bytes(256)),
        )
        events = []
        async for event in pipeline.process_round(audio=bytes(640), history=[]):
            events.append(event)

        types = [e.type for e in events]
        assert types.index("transcript_final") < types.index("llm_done")
        assert types.index("llm_done") < types.index("tts_start")
        assert types.index("tts_start") < types.index("tts_end")


class TestVoicePipelineData:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.2
    # @purpose 验证管道正确传递 STT 文字给 LLM，LLM 回复传递给 TTS
    # @context 数据流断裂（如 STT 文字未传给 LLM）会导致 LLM 收到空输入，
    #          产生无意义回复，用户体验完全失效
    @pytest.mark.asyncio
    async def test_stt_text_in_transcript_event(self):
        pipeline = VoicePipeline(
            stt_fn=AsyncMock(return_value="识别到的文字"),
            llm_fn=AsyncMock(return_value="ok"),
            tts_fn=AsyncMock(return_value=bytes(64)),
        )
        transcript_event = None
        async for event in pipeline.process_round(audio=bytes(640), history=[]):
            if event.type == "transcript_final":
                transcript_event = event
        assert transcript_event is not None
        assert transcript_event.data.get("text") == "识别到的文字"

    @pytest.mark.asyncio
    async def test_llm_receives_history(self):
        """LLM 调用时应携带对话历史"""
        llm_fn = AsyncMock(return_value="回复")
        pipeline = VoicePipeline(
            stt_fn=AsyncMock(return_value="新问题"),
            llm_fn=llm_fn,
            tts_fn=AsyncMock(return_value=bytes(64)),
        )
        history = [{"role": "user", "content": "之前的问题"}, {"role": "assistant", "content": "之前的回答"}]
        async for _ in pipeline.process_round(audio=bytes(640), history=history):
            pass
        # llm_fn 应收到包含历史的 messages
        call_args = llm_fn.call_args
        messages = call_args[0][0] if call_args[0] else call_args[1].get("messages", [])
        assert any(m["content"] == "之前的问题" for m in messages)


class TestVoicePipelineErrorHandling:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.4
    # @purpose 验证 STT/LLM 失败时发出 error 事件，不抛异常
    # @context 管道异常若未捕获会导致 WebSocket handler 崩溃，连接断开；
    #          正确处理应发送 error 事件，保持连接可用于下一轮
    @pytest.mark.asyncio
    async def test_stt_failure_emits_error_event(self):
        async def failing_stt(audio):
            raise RuntimeError("STT timeout")

        pipeline = VoicePipeline(
            stt_fn=failing_stt,
            llm_fn=AsyncMock(return_value="ok"),
            tts_fn=AsyncMock(return_value=bytes(64)),
        )
        events = []
        async for event in pipeline.process_round(audio=bytes(640), history=[]):
            events.append(event)

        types = [e.type for e in events]
        assert "error" in types
        error_event = next(e for e in events if e.type == "error")
        assert error_event.data.get("code") == "STT_FAIL"

    @pytest.mark.asyncio
    async def test_llm_failure_emits_error_event(self):
        async def failing_llm(messages):
            raise RuntimeError("LLM error")

        pipeline = VoicePipeline(
            stt_fn=AsyncMock(return_value="ok"),
            llm_fn=failing_llm,
            tts_fn=AsyncMock(return_value=bytes(64)),
        )
        events = []
        async for event in pipeline.process_round(audio=bytes(640), history=[]):
            events.append(event)

        types = [e.type for e in events]
        assert "error" in types
        error_event = next(e for e in events if e.type == "error")
        assert error_event.data.get("code") == "LLM_FAIL"

    @pytest.mark.asyncio
    async def test_empty_stt_result_skips_llm(self):
        """STT 返回空字符串时，不调用 LLM，直接结束"""
        llm_fn = AsyncMock(return_value="response")
        pipeline = VoicePipeline(
            stt_fn=AsyncMock(return_value=""),
            llm_fn=llm_fn,
            tts_fn=AsyncMock(return_value=bytes(64)),
        )
        async for _ in pipeline.process_round(audio=bytes(640), history=[]):
            pass
        llm_fn.assert_not_called()
