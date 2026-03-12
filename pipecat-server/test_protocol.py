"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §2
@purpose 验证 WebSocket 消息协议编解码的正确性
@context protocol.py 是 iOS ↔ Pipecat 通信的基础；
         如果此测试失败，说明消息格式不符合设计协议，
         iOS 将无法正确解析服务端推送，对话链路完全中断。
"""

import pytest
import json
from protocol import (
    encode_json,
    decode_message,
    encode_tts_audio,
    decode_tts_audio,
    MessageType,
    WebSocketMessage,
    TTS_AUDIO_PREFIX,
)


class TestEncodeJson:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §2.3
    # @purpose 验证 JSON 控制消息能被正确序列化为 bytes
    # @context encode_json 用于服务端向 iOS 发送所有非音频事件；
    #          序列化失败会导致 iOS 收不到 transcript/llm/tts 事件
    def test_encode_ready_message(self):
        data = encode_json({"type": "ready"})
        assert isinstance(data, bytes)
        assert json.loads(data) == {"type": "ready"}

    def test_encode_transcript_final(self):
        data = encode_json({"type": "transcript_final", "text": "你好"})
        parsed = json.loads(data)
        assert parsed["type"] == "transcript_final"
        assert parsed["text"] == "你好"

    def test_encode_llm_done(self):
        data = encode_json({"type": "llm_done", "text": "天气晴好"})
        parsed = json.loads(data)
        assert parsed["type"] == "llm_done"
        assert parsed["text"] == "天气晴好"

    def test_encode_error(self):
        data = encode_json({"type": "error", "code": "STT_FAIL", "message": "stt timeout"})
        parsed = json.loads(data)
        assert parsed["type"] == "error"
        assert parsed["code"] == "STT_FAIL"


class TestDecodeMessage:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §2.2
    # @purpose 验证 iOS 发来的 WebSocket 消息能被正确解析（JSON 控制消息 vs Binary 音频帧）
    # @context decode_message 是消息入口分发的核心；
    #          解析错误会导致 start/stop 指令被忽略，或音频帧被当成 JSON 解析崩溃
    def test_decode_json_start(self):
        raw = json.dumps({"type": "start"}).encode("utf-8")
        msg = decode_message(raw)
        assert msg.type == MessageType.START
        assert msg.is_control

    def test_decode_json_stop(self):
        raw = json.dumps({"type": "stop"}).encode("utf-8")
        msg = decode_message(raw)
        assert msg.type == MessageType.STOP
        assert msg.is_control

    def test_decode_json_ping(self):
        raw = json.dumps({"type": "ping"}).encode("utf-8")
        msg = decode_message(raw)
        assert msg.type == MessageType.PING
        assert msg.is_control

    def test_decode_binary_audio_frame(self):
        """640 字节原始 PCM 被识别为音频帧"""
        audio_data = bytes(640)
        msg = decode_message(audio_data)
        assert msg.type == MessageType.AUDIO
        assert msg.is_audio
        assert msg.payload == audio_data

    def test_decode_unknown_json_type(self):
        """未知 type 不抛异常，返回 UNKNOWN 类型"""
        raw = json.dumps({"type": "unknown_cmd"}).encode("utf-8")
        msg = decode_message(raw)
        assert msg.type == MessageType.UNKNOWN


class TestTtsAudioFrame:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §2.3
    # @purpose 验证 TTS 音频帧的 0xAA 前缀标识正确编解码
    # @context iOS 通过首字节 0xAA 区分 TTS 音频帧与 JSON 事件；
    #          前缀错误会导致 iOS 将音频数据当 JSON 解析崩溃，或忽略 TTS 音频
    def test_encode_tts_audio_adds_prefix(self):
        audio = bytes([0x01, 0x02, 0x03])
        framed = encode_tts_audio(audio)
        assert framed[0] == TTS_AUDIO_PREFIX
        assert framed[1:] == audio

    def test_decode_tts_audio_strips_prefix(self):
        audio = bytes([0x10, 0x20, 0x30])
        framed = bytes([TTS_AUDIO_PREFIX]) + audio
        decoded = decode_tts_audio(framed)
        assert decoded == audio

    def test_encode_decode_roundtrip(self):
        original = bytes(range(256)) * 4  # 1024 bytes
        assert decode_tts_audio(encode_tts_audio(original)) == original

    def test_tts_prefix_is_0xAA(self):
        assert TTS_AUDIO_PREFIX == 0xAA
