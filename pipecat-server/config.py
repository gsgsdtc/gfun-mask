"""
@doc     docs/modules/pipecat-pipeline/design/03-ios-voice-chat-pipecat-backend-design.md §5.1
@purpose 从 .env 文件加载服务配置，统一管理 LLM / STT / TTS API 参数
"""

import os
from dotenv import load_dotenv

load_dotenv()

# 阿里云 DashScope OpenAI 兼容接入点
DASHSCOPE_BASE_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1"

# LM Studio 本地模型接入点
LMSTUDIO_BASE_URL = os.getenv("LMSTUDIO_BASE_URL", "http://localhost:1234/v1")


class Config:
    # 服务地址
    SERVER_HOST: str = os.getenv("SERVER_HOST", "0.0.0.0")
    SERVER_PORT: int = int(os.getenv("SERVER_PORT", "8765"))

    # LLM 提供商选择: "dashscope" | "lmstudio"
    LLM_PROVIDER: str = os.getenv("LLM_PROVIDER", "dashscope")

    # 阿里云 DashScope API Key（STT / TTS 需要，LLM 可选）
    DASHSCOPE_API_KEY: str = os.getenv("DASHSCOPE_API_KEY", "")

    # LM Studio 配置（本地模型）
    LMSTUDIO_API_KEY: str = os.getenv("LMSTUDIO_API_KEY", "lm-studio")  # LM Studio 默认无需验证
    LMSTUDIO_MODEL: str = os.getenv("LMSTUDIO_MODEL", "local-model")   # LM Studio 自动加载的模型

    # STT：Paraformer（目前仅支持 DashScope）
    STT_MODEL: str = os.getenv("STT_MODEL", "paraformer-v2")

    # LLM 模型配置
    # 当 LLM_PROVIDER=dashscope 时使用 DASHSCOPE 模型
    # 当 LLM_PROVIDER=lmstudio 时使用 LMSTUDIO 模型
    LLM_MODEL: str = os.getenv("LLM_MODEL", "qwen-turbo-latest")
    LLM_SYSTEM_PROMPT: str = os.getenv(
        "LLM_SYSTEM_PROMPT",
        "你是一个简洁友好的语音助手，请用中文回答，每次回复不超过50字。"
    )

    # TTS：CosyVoice（目前仅支持 DashScope）
    TTS_MODEL: str = os.getenv("TTS_MODEL", "cosyvoice-v1")
    TTS_VOICE: str = os.getenv("TTS_VOICE", "longxiaochun")

    # 对话历史最大轮数
    MAX_HISTORY_TURNS: int = 10
