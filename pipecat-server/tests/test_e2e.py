"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §6.2
@purpose E2E 测试：通过真实 WebSocket 连接本地运行的 Pipecat 服务
         验证完整消息流（连接 → 发送音频 → 收到 STT/LLM/TTS 事件）
@context 使用真实 DashScope API，需要服务在 localhost:8765 运行
         运行前执行：python main.py &
"""

import asyncio
import json
import struct
import math
import pytest
import websockets

SERVER_URL = "ws://localhost:8765/ws"

# ──────────────────────────────────────────────
# 辅助：生成静音 PCM（640B × N 帧）
# ──────────────────────────────────────────────

def _make_silence_pcm(frames: int = 50) -> bytes:
    """生成 50 帧 × 640B 静音 PCM（320 samples × 2B, int16 zeros）"""
    return bytes(640 * frames)


def _make_tone_pcm(frames: int = 50, freq_hz: int = 440, sample_rate: int = 16000) -> bytes:
    """生成 50 帧 × 640B 440Hz 正弦波 PCM（用于调试，不是真实语音）"""
    samples_per_frame = 320
    result = bytearray()
    for f in range(frames):
        for s in range(samples_per_frame):
            t = (f * samples_per_frame + s) / sample_rate
            sample = int(32767 * 0.3 * math.sin(2 * math.pi * freq_hz * t))
            result += struct.pack("<h", sample)
    return bytes(result)


# ──────────────────────────────────────────────
# 辅助：收集所有 WebSocket 消息直到 tts_end 或 error 或超时
# ──────────────────────────────────────────────

async def _collect_events(ws, timeout: float = 30.0) -> list:
    events = []
    try:
        async with asyncio.timeout(timeout):
            while True:
                raw = await ws.recv()
                if isinstance(raw, bytes):
                    # Server sends JSON events as binary frames (UTF-8 encoded JSON bytes).
                    # TTS audio frames start with 0xAA; everything else is JSON.
                    first = raw[0] if raw else 0
                    if first in (ord("{"), ord("["), ord('"')):
                        try:
                            obj = json.loads(raw.decode("utf-8"))
                            events.append(obj)
                            t = obj.get("type", "")
                            if t in ("tts_end", "error"):
                                break
                            continue
                        except (json.JSONDecodeError, UnicodeDecodeError):
                            pass
                    events.append({"type": "__binary__", "size": len(raw)})
                else:
                    obj = json.loads(raw)
                    events.append(obj)
                    t = obj.get("type", "")
                    if t in ("tts_end", "error"):
                        break
    except TimeoutError:
        events.append({"type": "__timeout__"})
    return events


# ──────────────────────────────────────────────
# E2E 测试用例
# ──────────────────────────────────────────────

class TestE2EConnection:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §2.1
    # @purpose 验证 WebSocket 连接建立后服务端立即发送 ready 事件
    # @context 若 ready 事件缺失，iOS 端无法知晓服务已就绪，会卡在"服务器未连接"状态
    @pytest.mark.asyncio
    async def test_connection_receives_ready_event(self):
        async with websockets.connect(SERVER_URL) as ws:
            raw = await asyncio.wait_for(ws.recv(), timeout=5)
            msg = json.loads(raw)
            assert msg["type"] == "ready", f"Expected 'ready', got: {msg}"


class TestE2EPingPong:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §2.2
    # @purpose 验证 ping/pong 心跳保活机制
    # @context 长时间无对话时 iOS 需要 ping 保持连接；若无 pong 响应会误判断连
    @pytest.mark.asyncio
    async def test_ping_receives_pong(self):
        async with websockets.connect(SERVER_URL) as ws:
            await ws.recv()  # 消耗 ready 事件
            await ws.send(json.dumps({"type": "ping"}))
            raw = await asyncio.wait_for(ws.recv(), timeout=5)
            msg = json.loads(raw)
            assert msg["type"] == "pong"


class TestE2ESilenceRound:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.3
    # @purpose 验证静音帧场景：STT 返回空文本时不触发 LLM，直接结束
    # @context 用户没说话就停止录音时应静默结束，不产生无意义 AI 回复
    @pytest.mark.asyncio
    async def test_silence_emits_empty_transcript_no_llm(self):
        async with websockets.connect(SERVER_URL) as ws:
            await ws.recv()  # ready

            # 发送开始指令
            await ws.send(json.dumps({"type": "start"}))

            # 发送 10 帧静音 PCM
            silence = _make_silence_pcm(frames=10)
            chunk_size = 640
            for i in range(0, len(silence), chunk_size):
                await ws.send(silence[i:i+chunk_size])

            # 发送停止指令
            await ws.send(json.dumps({"type": "stop"}))

            # 收集事件
            events = await _collect_events(ws, timeout=20)
            types = [e.get("type") for e in events]
            print(f"[silence] events: {types}")

            # 应收到 transcript_final（text 可能为空）
            assert "transcript_final" in types, f"Expected transcript_final in {types}"

            # 不应触发 llm_done（静音无内容）
            assert "llm_done" not in types, f"LLM should not be called for silence, got: {types}"


class TestE2EFullRound:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.2
    # @purpose 验证完整对话轮次：发送真实 PCM 音频（含语音内容），收到 STT→LLM→TTS 完整事件序列
    # @context 这是核心链路验证；任何环节失败都意味着语音聊天功能无法使用
    @pytest.mark.asyncio
    async def test_full_round_with_real_audio(self):
        """
        使用预录的 WAV 文件（或生成的测试音频）发送给服务端，
        验证完整 STT → LLM → TTS 链路。

        注意：此测试需要 assets/test_hello.pcm 存在（16kHz 16-bit mono PCM）
        若文件不存在，改为发送 tone，验证 STT 识别结果可为空或有内容均通过。
        """
        import os
        pcm_path = os.path.join(os.path.dirname(__file__), "assets", "test_hello.pcm")
        if os.path.exists(pcm_path):
            with open(pcm_path, "rb") as f:
                audio_data = f.read()
        else:
            # fallback：发送正弦波（STT 结果可能为空）
            audio_data = _make_tone_pcm(frames=100)

        async with websockets.connect(SERVER_URL) as ws:
            await ws.recv()  # ready

            await ws.send(json.dumps({"type": "start"}))

            # 分帧发送（每帧 640B，模拟 BLE 实时推送）
            chunk_size = 640
            for i in range(0, len(audio_data), chunk_size):
                await ws.send(audio_data[i:i+chunk_size])
                await asyncio.sleep(0.02)  # 模拟 20ms 帧间隔

            await ws.send(json.dumps({"type": "stop"}))

            events = await _collect_events(ws, timeout=30)
            types = [e.get("type") for e in events]
            print(f"[full_round] events: {types}")
            print(f"[full_round] transcript: {next((e.get('text') for e in events if e.get('type') == 'transcript_final'), None)}")
            print(f"[full_round] llm_reply:  {next((e.get('text') for e in events if e.get('type') == 'llm_done'), None)}")
            print(f"[full_round] tts_audio_frames: {sum(1 for e in events if e.get('type') == '__binary__')}")

            # 必须收到 transcript_final
            assert "transcript_final" in types, f"Missing transcript_final in {types}"

            # 若有识别内容，必须有 LLM 回复和 TTS
            transcript = next((e.get("text", "") for e in events if e.get("type") == "transcript_final"), "")
            if transcript.strip():
                assert "llm_done" in types, f"Expected llm_done after non-empty transcript"
                assert "tts_start" in types, f"Expected tts_start"
                assert "tts_end" in types, f"Expected tts_end"
                assert any(e.get("type") == "__binary__" for e in events), "Expected TTS audio frames"


class TestE2EEventOrder:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.2
    # @purpose 验证事件顺序：transcript_final → llm_done → tts_start → tts_audio → tts_end
    # @context iOS 依赖事件顺序更新 UI 状态（recording→processing→playing→idle）；
    #          顺序错误会导致状态机卡住或 UI 显示混乱
    @pytest.mark.asyncio
    async def test_event_order_is_correct(self):
        import os
        pcm_path = os.path.join(os.path.dirname(__file__), "assets", "test_hello.pcm")
        if not os.path.exists(pcm_path):
            pytest.skip("assets/test_hello.pcm not found，跳过顺序验证测试")

        with open(pcm_path, "rb") as f:
            audio_data = f.read()

        async with websockets.connect(SERVER_URL) as ws:
            await ws.recv()  # ready
            await ws.send(json.dumps({"type": "start"}))

            for i in range(0, len(audio_data), 640):
                await ws.send(audio_data[i:i+640])
                await asyncio.sleep(0.02)

            await ws.send(json.dumps({"type": "stop"}))
            events = await _collect_events(ws, timeout=30)
            types = [e.get("type") for e in events if e.get("type") not in ("__binary__", None)]

            if "llm_done" not in types:
                pytest.skip("STT 未识别到内容，跳过顺序验证")

            assert types.index("transcript_final") < types.index("llm_done")
            assert types.index("llm_done") < types.index("tts_start")
            assert types.index("tts_start") < types.index("tts_end")


class TestE2EMultiRound:
    # @doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.3
    # @purpose 验证同一 WebSocket 连接内可进行多轮对话（state 正确复位）
    # @context 多轮对话是语音聊天的基本要求；若每轮后 state 未复位，第二轮将无法触发处理
    @pytest.mark.asyncio
    async def test_two_consecutive_silence_rounds(self):
        """两轮连续静音对话，均应收到 transcript_final，state 正常复位"""
        async with websockets.connect(SERVER_URL) as ws:
            await ws.recv()  # ready

            for round_num in range(2):
                await ws.send(json.dumps({"type": "start"}))
                silence = _make_silence_pcm(frames=10)
                for i in range(0, len(silence), 640):
                    await ws.send(silence[i:i+640])
                await ws.send(json.dumps({"type": "stop"}))

                events = await _collect_events(ws, timeout=20)
                types = [e.get("type") for e in events]
                print(f"[round {round_num+1}] events: {types}")
                assert "transcript_final" in types, f"Round {round_num+1}: missing transcript_final"
