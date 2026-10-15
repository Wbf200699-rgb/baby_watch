from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    root_dir: Path
    data_dir: Path
    database_path: Path
    artifact_dir: Path
    device_keys: dict[str, str]
    offline_after_seconds: int = 10
    command_redelivery_seconds: int = 5
    telemetry_retention_rows: int = 20000
    log_retention_rows: int = 10000
    matrix_retention_rows: int = 2000
    max_artifact_bytes: int = 20 * 1024 * 1024
    allow_artifact_upload: bool = False
    admin_user: str = ""
    admin_password: str = ""
    video_target_fps: int = 8
    ai_enabled: bool = False
    dashscope_api_key: str = ""
    qwen_chat_url: str = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
    qwen_model: str = "qwen-plus"
    qwen_tts_url: str = "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer"
    qwen_tts_model: str = "qwen-audio-3.0-tts-flash"
    qwen_tts_voice: str = "longanhuan_v3.6"
    ai_assessment_interval_seconds: int = 600
    ai_event_cooldown_seconds: int = 60
    ai_story_max_bytes: int = 4 * 1024 * 1024

    @classmethod
    def from_env(cls, root_dir: Path | None = None) -> "Settings":
        root = (root_dir or Path(__file__).resolve().parents[1]).resolve()
        data_dir = Path(os.getenv("BABY_MONITOR_DATA_DIR", root / "data")).resolve()
        raw_keys = os.getenv("BABY_MONITOR_DEVICE_KEYS", "demo-device:dev-secret")
        keys: dict[str, str] = {}
        for entry in raw_keys.split(","):
            device_id, separator, key = entry.strip().partition(":")
            if separator and device_id and key:
                keys[device_id] = key
        if not keys:
            raise ValueError("BABY_MONITOR_DEVICE_KEYS must contain device:key pairs")
        video_target_fps = int(os.getenv("BABY_MONITOR_VIDEO_TARGET_FPS", "8"))
        if not 8 <= video_target_fps <= 12:
            raise ValueError("BABY_MONITOR_VIDEO_TARGET_FPS must be between 8 and 12")
        dashscope_api_key = os.getenv("DASHSCOPE_API_KEY", "").strip()
        ai_enabled = os.getenv("BABY_MONITOR_AI_ENABLED", "0") == "1"
        if ai_enabled and not dashscope_api_key:
            raise ValueError("DASHSCOPE_API_KEY is required when BABY_MONITOR_AI_ENABLED=1")
        return cls(
            root_dir=root,
            data_dir=data_dir,
            database_path=data_dir / "baby_monitor.sqlite3",
            artifact_dir=data_dir / "artifacts",
            device_keys=keys,
            offline_after_seconds=int(os.getenv("BABY_MONITOR_OFFLINE_SECONDS", "10")),
            command_redelivery_seconds=int(os.getenv("BABY_MONITOR_COMMAND_REDELIVERY_SECONDS", "5")),
            telemetry_retention_rows=int(os.getenv("BABY_MONITOR_TELEMETRY_RETENTION_ROWS", "20000")),
            log_retention_rows=int(os.getenv("BABY_MONITOR_LOG_RETENTION_ROWS", "10000")),
            matrix_retention_rows=int(os.getenv("BABY_MONITOR_MATRIX_RETENTION_ROWS", "2000")),
            max_artifact_bytes=int(os.getenv("BABY_MONITOR_MAX_ARTIFACT_BYTES", str(20 * 1024 * 1024))),
            allow_artifact_upload=os.getenv("BABY_MONITOR_ALLOW_ARTIFACT_UPLOAD", "0") == "1",
            admin_user=os.getenv("BABY_MONITOR_ADMIN_USER", ""),
            admin_password=os.getenv("BABY_MONITOR_ADMIN_PASSWORD", ""),
            video_target_fps=video_target_fps,
            ai_enabled=ai_enabled,
            dashscope_api_key=dashscope_api_key,
            qwen_chat_url=os.getenv(
                "BABY_MONITOR_QWEN_CHAT_URL",
                "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
            ),
            qwen_model=os.getenv("BABY_MONITOR_QWEN_MODEL", "qwen-plus"),
            qwen_tts_url=os.getenv(
                "BABY_MONITOR_QWEN_TTS_URL",
                "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer",
            ),
            qwen_tts_model=os.getenv(
                "BABY_MONITOR_QWEN_TTS_MODEL", "qwen-audio-3.0-tts-flash"
            ),
            qwen_tts_voice=os.getenv("BABY_MONITOR_QWEN_TTS_VOICE", "longanhuan_v3.6"),
            ai_assessment_interval_seconds=max(
                60, int(os.getenv("BABY_MONITOR_AI_ASSESSMENT_INTERVAL_SECONDS", "600"))
            ),
            ai_event_cooldown_seconds=max(
                15, int(os.getenv("BABY_MONITOR_AI_EVENT_COOLDOWN_SECONDS", "60"))
            ),
            ai_story_max_bytes=max(
                64 * 1024,
                min(
                    8 * 1024 * 1024,
                    int(os.getenv("BABY_MONITOR_AI_STORY_MAX_BYTES", str(4 * 1024 * 1024))),
                ),
            ),
        )
