"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §3, §4.3
@purpose 每个 WebSocket 连接的会话状态机和上下文管理
"""

from __future__ import annotations
import uuid
from enum import Enum
from typing import List, Dict
from config import Config


class SessionState(Enum):
    IDLE = "idle"
    RECEIVING = "receiving"
    PROCESSING = "processing"
    SPEAKING = "speaking"


class Session:
    def __init__(
        self,
        system_prompt: str = Config.LLM_SYSTEM_PROMPT,
        max_history_turns: int = Config.MAX_HISTORY_TURNS,
    ):
        self.session_id: str = str(uuid.uuid4())
        self.state: SessionState = SessionState.IDLE
        self.audio_buffer: bytes = b""
        self.conversation_history: List[Dict[str, str]] = []
        self._system_prompt: str = system_prompt
        self._max_history_turns: int = max_history_turns

    # ──────────────────── 状态流转 ────────────────────

    def start_recording(self) -> None:
        """IDLE → RECEIVING：开始接收 PCM 音频帧"""
        self.state = SessionState.RECEIVING

    def stop_recording(self) -> None:
        """RECEIVING → PROCESSING：停止收音，触发 STT/LLM/TTS"""
        self.state = SessionState.PROCESSING

    def start_speaking(self) -> None:
        """PROCESSING → SPEAKING：TTS 开始播放"""
        self.state = SessionState.SPEAKING

    def finish_round(self) -> None:
        """SPEAKING → IDLE：本轮结束，清空音频缓冲区"""
        self.state = SessionState.IDLE
        self.audio_buffer = b""

    def set_error(self) -> None:
        """任意状态 → IDLE：管道异常，重置状态"""
        self.state = SessionState.IDLE
        self.audio_buffer = b""

    # ──────────────────── 音频缓冲 ────────────────────

    def append_audio(self, data: bytes) -> None:
        """追加 PCM 音频帧到缓冲区"""
        self.audio_buffer += data

    # ──────────────────── 对话历史 ────────────────────

    def add_message(self, role: str, content: str) -> None:
        """
        追加一条消息到对话历史。
        如果超过 max_history_turns 轮，裁剪最早的 user+assistant 对。
        """
        self.conversation_history.append({"role": role, "content": content})
        # 每轮 = user + assistant 各 1 条 = 2 条
        max_messages = self._max_history_turns * 2
        if len(self.conversation_history) > max_messages:
            # 裁剪最早的 2 条（一轮）
            self.conversation_history = self.conversation_history[2:]

    def get_messages_for_llm(self) -> List[Dict[str, str]]:
        """返回带 system prompt 的完整 messages 列表，用于 LLM API 调用。"""
        system = [{"role": "system", "content": self._system_prompt}]
        return system + self.conversation_history
