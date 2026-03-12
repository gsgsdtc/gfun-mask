"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §2
@purpose WebSocket 消息协议编解码：JSON 控制消息 + Binary 音频帧（TTS 以 0xAA 前缀标识）
"""

from __future__ import annotations
import json
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

# TTS 音频帧的首字节标识符（iOS 通过此字节区分 TTS 音频 vs JSON 事件）
TTS_AUDIO_PREFIX: int = 0xAA


class MessageType(Enum):
    START = "start"
    STOP = "stop"
    PING = "ping"
    AUDIO = "__audio__"
    UNKNOWN = "__unknown__"


@dataclass
class WebSocketMessage:
    type: MessageType
    payload: Optional[bytes] = None
    data: dict = field(default_factory=dict)

    @property
    def is_control(self) -> bool:
        return self.type in (MessageType.START, MessageType.STOP, MessageType.PING)

    @property
    def is_audio(self) -> bool:
        return self.type == MessageType.AUDIO


def encode_json(obj: dict) -> bytes:
    """将字典序列化为 UTF-8 JSON bytes，用于服务端 → iOS 的所有非音频消息。"""
    return json.dumps(obj, ensure_ascii=False).encode("utf-8")


def decode_message(raw: bytes) -> WebSocketMessage:
    """
    解析 iOS 发来的 WebSocket 消息。
    - 若首字节对应合法 JSON 起始字符（{ [ " 等），尝试 JSON 解析 → 控制消息
    - 否则视为二进制音频帧
    """
    if not raw:
        return WebSocketMessage(type=MessageType.UNKNOWN)

    # 尝试 JSON 解析（JSON 必须以 { 或 [ 开头，ASCII < 128 的常见起始字符）
    first = raw[0]
    if first in (ord("{"), ord("["), ord('"')):
        try:
            obj = json.loads(raw.decode("utf-8"))
            msg_type_str = obj.get("type", "")
            try:
                msg_type = MessageType(msg_type_str)
            except ValueError:
                msg_type = MessageType.UNKNOWN
            return WebSocketMessage(type=msg_type, data=obj)
        except (json.JSONDecodeError, UnicodeDecodeError):
            pass

    # 二进制音频帧
    return WebSocketMessage(type=MessageType.AUDIO, payload=raw)


def encode_tts_audio(audio_bytes: bytes) -> bytes:
    """在 TTS 音频数据前添加 0xAA 标识前缀，iOS 通过首字节识别音频帧。"""
    return bytes([TTS_AUDIO_PREFIX]) + audio_bytes


def decode_tts_audio(framed: bytes) -> bytes:
    """去除 TTS 帧的 0xAA 前缀，返回原始音频数据。"""
    return framed[1:]
