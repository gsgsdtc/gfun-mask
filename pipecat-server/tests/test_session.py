"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §3, §4.3
@purpose 验证会话状态机的状态流转和对话上下文管理
@context session.py 是每个 WebSocket 连接的核心状态容器；
         如果状态机逻辑错误，会导致多轮对话状态混乱、音频缓冲区未清理、
         历史超限未裁剪等问题，最终使对话链路失效。
"""

import pytest
from core.session import Session, SessionState


class TestSessionInitialState:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §3.1
    # @purpose 验证会话初始化时状态和缓冲区均为空
    # @context 每个 WebSocket 连接建立时创建新 Session；
    #          初始化不正确会导致上一次连接的残留数据污染新会话
    def test_initial_state_is_idle(self):
        s = Session()
        assert s.state == SessionState.IDLE

    def test_initial_audio_buffer_empty(self):
        s = Session()
        assert s.audio_buffer == b""

    def test_initial_conversation_history_empty(self):
        s = Session()
        assert s.conversation_history == []

    def test_session_id_is_string(self):
        s = Session()
        assert isinstance(s.session_id, str)
        assert len(s.session_id) > 0


class TestSessionStateTransitions:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §3.2
    # @purpose 验证会话状态机按设计正确流转
    # @context 状态机控制整个对话轮次的生命周期；
    #          非法状态转换（如 IDLE→PROCESSING 跳过 RECEIVING）会导致音频还未收完就触发 STT
    def test_start_transitions_to_receiving(self):
        s = Session()
        s.start_recording()
        assert s.state == SessionState.RECEIVING

    def test_stop_transitions_to_processing(self):
        s = Session()
        s.start_recording()
        s.stop_recording()
        assert s.state == SessionState.PROCESSING

    def test_set_processing_transitions_state(self):
        s = Session()
        s.start_recording()
        s.stop_recording()
        assert s.state == SessionState.PROCESSING

    def test_set_speaking_transitions_state(self):
        s = Session()
        s.start_recording()
        s.stop_recording()
        s.start_speaking()
        assert s.state == SessionState.SPEAKING

    def test_finish_round_resets_to_idle(self):
        s = Session()
        s.start_recording()
        s.stop_recording()
        s.start_speaking()
        s.finish_round()
        assert s.state == SessionState.IDLE

    def test_reset_to_idle_clears_audio_buffer(self):
        s = Session()
        s.start_recording()
        s.append_audio(bytes(640))
        s.stop_recording()
        s.start_speaking()
        s.finish_round()
        assert s.audio_buffer == b""

    def test_error_resets_to_idle(self):
        s = Session()
        s.start_recording()
        s.stop_recording()
        s.set_error()
        assert s.state == SessionState.IDLE


class TestSessionAudioBuffer:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.2
    # @purpose 验证音频帧被正确累积到缓冲区
    # @context PCM 帧每帧 640B，多帧累积后交给 STT；
    #          如果 append_audio 逻辑错误，STT 收到的音频不完整，识别结果为空或错误
    def test_append_audio_accumulates(self):
        s = Session()
        s.start_recording()
        s.append_audio(bytes(640))
        s.append_audio(bytes(640))
        assert len(s.audio_buffer) == 1280

    def test_get_audio_returns_buffer(self):
        s = Session()
        s.start_recording()
        data = bytes([0x01] * 640)
        s.append_audio(data)
        assert s.audio_buffer == data


class TestConversationHistory:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.3
    # @purpose 验证对话历史的追加和超限裁剪逻辑
    # @context 历史记录作为 LLM context 使用；超限不裁剪会导致 LLM API 请求超出 token 限制
    def test_add_user_message(self):
        s = Session()
        s.add_message("user", "你好")
        assert s.conversation_history == [{"role": "user", "content": "你好"}]

    def test_add_assistant_message(self):
        s = Session()
        s.add_message("user", "你好")
        s.add_message("assistant", "你好，有什么可以帮你？")
        assert len(s.conversation_history) == 2
        assert s.conversation_history[1]["role"] == "assistant"

    def test_history_trimmed_when_exceeds_max_turns(self):
        """超过 MAX_HISTORY_TURNS 轮时，裁剪最早的 user+assistant 对"""
        s = Session(max_history_turns=2)
        # 添加 3 轮（6 条消息）
        for i in range(3):
            s.add_message("user", f"问题{i}")
            s.add_message("assistant", f"回答{i}")
        # 应只保留最近 2 轮（4 条）
        assert len(s.conversation_history) == 4
        assert s.conversation_history[0]["content"] == "问题1"

    def test_history_not_trimmed_within_limit(self):
        s = Session(max_history_turns=3)
        for i in range(3):
            s.add_message("user", f"q{i}")
            s.add_message("assistant", f"a{i}")
        assert len(s.conversation_history) == 6

    def test_get_messages_for_llm_includes_system(self):
        """get_messages_for_llm 返回带 system prompt 的完整 messages 列表"""
        s = Session(system_prompt="你是助手")
        s.add_message("user", "你好")
        messages = s.get_messages_for_llm()
        assert messages[0] == {"role": "system", "content": "你是助手"}
        assert messages[1] == {"role": "user", "content": "你好"}
