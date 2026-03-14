"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.1
@purpose 阿里云 DashScope 的 Pipecat STT / TTS 自定义服务
         STT: paraformer-realtime-v2，通过 Recognition.call() 批量识别
         TTS: cosyvoice-v1，通过 SpeechSynthesizer.call() 合成 MP3
"""

from __future__ import annotations

import asyncio
import os
import struct
import tempfile
import time
from typing import AsyncGenerator, Optional

import dashscope
from loguru import logger

from latency import LatencyRecord

from pipecat.frames.frames import (
    AudioRawFrame,
    ErrorFrame,
    Frame,
    TTSAudioRawFrame,
    TTSStartedFrame,
    TTSStoppedFrame,
    TranscriptionFrame,
)
from pipecat.services.stt_service import SegmentedSTTService, STTSettings
from pipecat.services.tts_service import TTSService, TTSSettings
from pipecat.transcriptions.language import Language


# ──────────────────────────────────────────────
# 辅助：将原始 PCM 封装为 WAV
# ──────────────────────────────────────────────

def _pcm_to_wav(pcm_bytes: bytes, sample_rate: int = 16000,
                channels: int = 1, bits: int = 16) -> bytes:
    byte_rate = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    data_size = len(pcm_bytes)
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + data_size, b"WAVE", b"fmt ",
        16, 1, channels, sample_rate, byte_rate, block_align, bits,
        b"data", data_size,
    )
    return header + pcm_bytes


# ──────────────────────────────────────────────
# DashScope STT：paraformer-realtime-v2
# ──────────────────────────────────────────────

class DashScopeSTTService(SegmentedSTTService):
    """
    阿里云 Paraformer STT 的 Pipecat 服务实现。
    接收累积的 PCM 音频 bytes，转写后 yield TranscriptionFrame。
    """

    def __init__(self, *, api_key: str, model: str = "paraformer-realtime-v2",
                 record: Optional[LatencyRecord] = None, **kwargs):
        super().__init__(settings=STTSettings(model=None, language=None), **kwargs)
        self._api_key = api_key
        self._model = model
        self._record = record

    async def run_stt(self, audio: bytes) -> AsyncGenerator[Frame, None]:
        # SegmentedSTTService 传入的已经是 WAV bytes（含 header）
        if not audio:
            logger.warning("[DashScopeSTT] 收到空音频，跳过")
            return

        if self._record:
            self._record.asr_start = time.monotonic()
        logger.info(f"[DashScopeSTT] ▶ 开始识别：{len(audio)} bytes WAV")
        dashscope.api_key = self._api_key

        # 调试：保存 WAV 以便检查音频内容
        debug_wav_path = "/tmp/debug_stt_input.wav"
        with open(debug_wav_path, "wb") as dbg:
            dbg.write(audio)
        logger.info(f"[DashScopeSTT] 调试 WAV 已保存至 {debug_wav_path}，大小={len(audio)} bytes")

        def _sync_recognize(wav_path: str) -> str:
            from dashscope.audio.asr import Recognition, RecognitionCallback

            class _CB(RecognitionCallback):
                pass

            logger.info(f"[DashScopeSTT] 调用 DashScope API，model={self._model}，文件={wav_path}")
            rec = Recognition(
                model=self._model,
                callback=_CB(),
                format="wav",
                sample_rate=16000,
            )
            resp = rec.call(wav_path)
            logger.info(f"[DashScopeSTT] API 返回：status_code={resp.status_code}, output={resp.output}, message={getattr(resp, 'message', '')}")
            if resp.status_code != 200:
                raise RuntimeError(f"STT failed [{resp.status_code}]: {resp.message}")
            sentence_list = (resp.output or {}).get("sentence") or []
            logger.info(f"[DashScopeSTT] sentence_list type={type(sentence_list)}, value={sentence_list}")
            if isinstance(sentence_list, list):
                return " ".join(s.get("text", "") for s in sentence_list if s.get("text")).strip()
            if isinstance(sentence_list, dict):
                return sentence_list.get("text", "").strip()
            return ""

        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            f.write(audio)
            tmp_path = f.name

        try:
            loop = asyncio.get_event_loop()
            text = await loop.run_in_executor(None, _sync_recognize, tmp_path)
        except Exception as e:
            logger.error(f"[DashScopeSTT] 识别失败: {e}", exc_info=True)
            yield ErrorFrame(f"STT_FAIL: {e}")
            return
        finally:
            os.unlink(tmp_path)

        if text:
            if self._record:
                t = time.monotonic()
                self._record.asr_end = t
                self._record.asr_first = t  # 批量模式：首包 = 总结束
            logger.info(f"[DashScopeSTT] ✓ 识别结果: '{text}'")
            yield TranscriptionFrame(text=text, user_id="user", timestamp="")
        else:
            logger.warning("[DashScopeSTT] 识别结果为空，发送 error 通知客户端")
            yield ErrorFrame("STT_EMPTY: 未能识别到语音，请靠近麦克风后重试")


# ──────────────────────────────────────────────
# DashScope TTS：cosyvoice-v1
# ──────────────────────────────────────────────

class DashScopeTTSService(TTSService):
    """
    阿里云 CosyVoice TTS 的 Pipecat 服务实现。
    接收文本，合成 MP3，yield TTSAudioRawFrame。
    """

    def __init__(self, *, api_key: str, model: str = "cosyvoice-v1",
                 voice: str = "longxiaochun", **kwargs):
        super().__init__(settings=TTSSettings(model=None, voice=None, language=None), **kwargs)
        self._api_key = api_key
        self._model = model
        self._voice = voice

    async def run_tts(self, text: str, context_id: str) -> AsyncGenerator[Frame, None]:
        logger.info(f"[DashScopeTTS] ▶ 开始合成：'{text[:80]}...' (model={self._model}, voice={self._voice})")
        dashscope.api_key = self._api_key

        def _sync_tts() -> bytes:
            import dashscope.audio.tts_v2 as tts_sdk
            synthesizer = tts_sdk.SpeechSynthesizer(
                model=self._model,
                voice=self._voice,
                format=tts_sdk.AudioFormat.MP3_22050HZ_MONO_256KBPS,
            )
            logger.info("[DashScopeTTS] 调用 DashScope TTS API...")
            audio = synthesizer.call(text)
            if not audio:
                raise RuntimeError("TTS returned empty audio")
            result = bytes(audio)
            logger.info(f"[DashScopeTTS] API 返回：{len(result)} bytes MP3")
            return result

        try:
            loop = asyncio.get_event_loop()
            mp3_bytes = await loop.run_in_executor(None, _sync_tts)
        except Exception as e:
            logger.error(f"[DashScopeTTS] 合成失败: {e}", exc_info=True)
            yield ErrorFrame(f"TTS_FAIL: {e}")
            return

        chunk_size = 4096
        chunk_count = (len(mp3_bytes) + chunk_size - 1) // chunk_size
        logger.info(f"[DashScopeTTS] ✓ 合成完成：{len(mp3_bytes)} bytes，分 {chunk_count} 块推送")
        yield TTSStartedFrame()
        for i in range(0, len(mp3_bytes), chunk_size):
            chunk = mp3_bytes[i:i + chunk_size]
            yield TTSAudioRawFrame(audio=chunk, sample_rate=22050, num_channels=1)
        yield TTSStoppedFrame()
        logger.info("[DashScopeTTS] ■ 所有音频块已推送")
