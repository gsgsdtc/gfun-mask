"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §4.1
@purpose 封装阿里云 DashScope STT / LLM / TTS 服务
         STT/TTS 使用 DashScope 原生 SDK（OpenAI 兼容接口不支持音频）
         LLM 使用 DashScope OpenAI 兼容接口

接口说明：
  STT  paraformer-realtime-v2，通过 Recognition.call() 批量识别，< 1s 返回
  LLM  qwen-turbo，通过 OpenAI 兼容接口（/compatible-mode/v1/chat/completions）
  TTS  cosyvoice-v1，通过 SpeechSynthesizer.call() 合成 MP3
"""

from __future__ import annotations
import struct
from typing import List, Dict
from config import Config, DASHSCOPE_BASE_URL


# ──────────────────────────────────────────────
# 辅助：将原始 PCM 16kHz 16-bit mono 封装为 WAV
# DashScope STT 接口需要带格式头的音频文件
# ──────────────────────────────────────────────

def _pcm_to_wav(pcm_bytes: bytes, sample_rate: int = 16000, channels: int = 1, bits: int = 16) -> bytes:
    """将裸 PCM bytes 封装成标准 RIFF WAV bytes。"""
    byte_rate = sample_rate * channels * bits // 8
    block_align = channels * bits // 8
    data_size = len(pcm_bytes)
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + data_size,
        b"WAVE",
        b"fmt ",
        16,           # PCM chunk size
        1,            # PCM format
        channels,
        sample_rate,
        byte_rate,
        block_align,
        bits,
        b"data",
        data_size,
    )
    return header + pcm_bytes


# ──────────────────────────────────────────────
# STT：阿里云 Paraformer（DashScope 原生 SDK）
# DashScope OpenAI 兼容接口不支持 audio/transcriptions，
# 需要使用原生 SDK — Recognition.call() 自动处理本地文件上传
# ──────────────────────────────────────────────

async def stt_dashscope(audio_bytes: bytes) -> str:
    """
    Paraformer 实时语音识别：原始 PCM → 转写文字
    使用 paraformer-realtime-v2 的同步 Recognition.call()，
    将整个录音文件一次性发送，通常 < 1s 返回。

    注意：DashScope OpenAI 兼容接口不支持音频转写；
          异步批量转写（Transcription）速度慢（> 30s），不适合语音助手场景；
          采用实时识别接口（Recognition）进行批量文件识别，速度更快。
    """
    import asyncio
    import os
    import tempfile
    import dashscope
    from dashscope.audio.asr import Recognition, RecognitionCallback

    dashscope.api_key = Config.DASHSCOPE_API_KEY
    wav_bytes = _pcm_to_wav(audio_bytes)

    def _sync_recognize(wav_path: str) -> str:
        class _Callback(RecognitionCallback):
            pass  # no streaming events needed for batch file recognition

        rec = Recognition(
            model="paraformer-realtime-v2",
            callback=_Callback(),
            format="wav",
            sample_rate=16000,
        )
        resp = rec.call(wav_path)
        if resp.status_code not in (200,):
            raise RuntimeError(f"STT failed [{resp.status_code}]: {resp.message}")

        # result.output["sentence"] is a list of sentence dicts after batch recognition
        sentence_list = (resp.output or {}).get("sentence") or []
        if isinstance(sentence_list, list):
            return " ".join(s.get("text", "") for s in sentence_list if s.get("text")).strip()
        # fallback: single sentence dict (streaming mode)
        if isinstance(sentence_list, dict):
            return sentence_list.get("text", "").strip()
        return ""

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(wav_bytes)
        tmp_path = f.name

    try:
        loop = asyncio.get_event_loop()
        text = await loop.run_in_executor(None, _sync_recognize, tmp_path)
    finally:
        os.unlink(tmp_path)

    return text


# ──────────────────────────────────────────────
# LLM：通义千问（OpenAI 兼容接口）
# ──────────────────────────────────────────────

async def llm_dashscope(messages: List[Dict]) -> str:
    """
    通义千问对话：messages → 回复文字
    - 使用 DashScope OpenAI 兼容接口，直接复用 openai SDK
    """
    from openai import AsyncOpenAI

    client = AsyncOpenAI(
        api_key=Config.DASHSCOPE_API_KEY,
        base_url=DASHSCOPE_BASE_URL,
    )
    response = await client.chat.completions.create(
        model=Config.LLM_MODEL,
        messages=messages,
    )
    return response.choices[0].message.content or ""


# ──────────────────────────────────────────────
# TTS：阿里云 CosyVoice（DashScope 原生 SDK）
# DashScope OpenAI 兼容接口不支持 audio/speech，
# 需要使用原生 SDK — SpeechSynthesizer.call() 返回 MP3 bytes
# ──────────────────────────────────────────────

async def tts_dashscope(text: str) -> bytes:
    """
    CosyVoice 语音合成：文字 → MP3 音频 bytes
    - 使用 DashScope 原生 SDK（SpeechSynthesizer），
      OpenAI 兼容接口不支持 audio/speech
    - SpeechSynthesizer.call() 为同步 I/O，用 run_in_executor 不阻塞事件循环
    - iOS 端用 AVAudioPlayer 直接播放 MP3
    """
    import asyncio
    import dashscope
    import dashscope.audio.tts_v2 as tts_sdk

    dashscope.api_key = Config.DASHSCOPE_API_KEY

    def _sync_tts() -> bytes:
        synthesizer = tts_sdk.SpeechSynthesizer(
            model=Config.TTS_MODEL,
            voice=Config.TTS_VOICE,
            format=tts_sdk.AudioFormat.MP3_22050HZ_MONO_256KBPS,
        )
        audio = synthesizer.call(text)
        if not audio:
            raise RuntimeError("TTS returned empty audio")
        return bytes(audio)

    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, _sync_tts)
