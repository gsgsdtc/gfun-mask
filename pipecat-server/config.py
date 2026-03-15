"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §5.1
@purpose 从 .env 文件加载服务配置，统一管理阿里云 DashScope API 参数
"""

import os
from dotenv import load_dotenv

load_dotenv()

# 阿里云 DashScope OpenAI 兼容接入点
DASHSCOPE_BASE_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1"


class Config:
    # 服务地址
    SERVER_HOST: str = os.getenv("SERVER_HOST", "0.0.0.0")
    SERVER_PORT: int = int(os.getenv("SERVER_PORT", "8765"))

    # 阿里云 DashScope API Key（STT / LLM / TTS 共用）
    DASHSCOPE_API_KEY: str = os.getenv("DASHSCOPE_API_KEY", "")

    # STT：Paraformer
    STT_MODEL: str = os.getenv("STT_MODEL", "paraformer-v2")

    # LLM：通义千问
    LLM_MODEL: str = os.getenv("LLM_MODEL", "qwen-turbo-latest")
    LLM_SYSTEM_PROMPT: str = os.getenv(
        "LLM_SYSTEM_PROMPT",
        "你是一个简洁友好的语音助手，请用中文回答，每次回复不超过50字。"
    )

    # TTS：CosyVoice
    TTS_MODEL: str = os.getenv("TTS_MODEL", "cosyvoice-v1")
    TTS_VOICE: str = os.getenv("TTS_VOICE", "longxiaochun")

    # 对话历史最大轮数
    MAX_HISTORY_TURNS: int = 10
