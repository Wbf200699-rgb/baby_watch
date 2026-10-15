from __future__ import annotations

import asyncio
import binascii
import hashlib
import json
import re
import struct
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Awaitable, Callable

import httpx

from .config import Settings
from .db import Database, utc_now


PublishCallback = Callable[[str, str, dict[str, Any]], Awaitable[None]]
STORY_QUEUE_LIMIT = 4
ASSESSMENT_QUEUE_LIMIT = 8
STORY_FILENAME = re.compile(r"^AI[0-9A-F]{6}\.WAV$")
STORY_TTS_CHUNK_CHARS = 260
STORY_CHARS_PER_SECOND = 4.2
STORY_SAMPLE_RATES = (8000, 16000)
STORY_PREFER_8K_BYTES = 2 * 1024 * 1024
VISION_LABELS = ("infant", "head_face", "blanket", "soft_object", "caregiver")
VISION_STATUS_LABELS = (
    "boot", "waiting_frame", "decoding", "preprocessing", "npu_starting",
    "inferencing", "ready", "jpeg_too_large", "jpeg_error", "npu_error", "ospi_error",
)
THERMAL_QUALITY_LABELS = ("normal", "transient_filtered", "invalid")


class AiServiceError(RuntimeError):
    pass


def _json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _loads(value: str | None, fallback: Any) -> Any:
    if not value:
        return fallback
    try:
        return json.loads(value)
    except (json.JSONDecodeError, TypeError):
        return fallback


def _clip_text(value: Any, maximum: int) -> str:
    return str(value or "").strip()[:maximum]


def _parse_json_object(content: Any) -> dict[str, Any]:
    if isinstance(content, list):
        content = "".join(
            str(item.get("text", "")) if isinstance(item, dict) else str(item)
            for item in content
        )
    text = str(content or "").strip()
    if text.startswith("```"):
        text = re.sub(r"^```(?:json)?\s*", "", text, flags=re.IGNORECASE)
        text = re.sub(r"\s*```$", "", text)
    try:
        result = json.loads(text)
    except json.JSONDecodeError as error:
        raise AiServiceError("model returned invalid JSON") from error
    if not isinstance(result, dict):
        raise AiServiceError("model response must be a JSON object")
    return result


def _parse_story_wav(data: bytes) -> tuple[int, bytes]:
    """Read PCM from canonical WAVs and DashScope streaming-size WAVs."""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AiServiceError("TTS response is not a valid WAV file")
    offset = 12
    sample_rate = 0
    format_ok = False
    pcm = b""
    while offset + 8 <= len(data):
        chunk_id = data[offset:offset + 4]
        declared = struct.unpack_from("<I", data, offset + 4)[0]
        content = offset + 8
        available = len(data) - content
        if chunk_id == b"fmt ":
            if available < 16 or declared < 16:
                raise AiServiceError("TTS WAV format chunk is incomplete")
            audio_format, channels, rate = struct.unpack_from("<HHI", data, content)
            sample_width = struct.unpack_from("<H", data, content + 14)[0]
            format_ok = audio_format == 1 and channels == 1 and sample_width == 16
            sample_rate = rate
        elif chunk_id == b"data":
            # DashScope may use 0x7ffffxxx as a streaming placeholder. The HTTP
            # body is complete, so the physical payload length is authoritative.
            physical = min(declared, available) & ~1
            pcm = bytes(data[content:content + physical])
            if declared > available:
                break
        if declared > available:
            raise AiServiceError("TTS WAV chunk exceeds the downloaded file")
        offset = content + declared + (declared & 1)
    if not format_ok or sample_rate not in STORY_SAMPLE_RATES:
        raise AiServiceError("TTS WAV must be PCM16LE, mono, 8000 or 16000 Hz")
    if not pcm:
        raise AiServiceError("TTS WAV contains no audio frames")
    return sample_rate, pcm


def _build_story_wav(sample_rate: int, pcm: bytes) -> bytes:
    if sample_rate not in STORY_SAMPLE_RATES or not pcm or len(pcm) & 1:
        raise AiServiceError("story PCM parameters are invalid")
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(pcm), b"WAVE", b"fmt ", 16, 1, 1,
        sample_rate, sample_rate * 2, 2, 16, b"data", len(pcm),
    ) + pcm


def _downsample_16k_to_8k(pcm: bytes) -> bytes:
    output = bytearray((len(pcm) // 4) * 2)
    target = 0
    for source in range(0, len(pcm) - 3, 4):
        first, second = struct.unpack_from("<hh", pcm, source)
        struct.pack_into("<h", output, target, (first + second) // 2)
        target += 2
    return bytes(output)


def _normalize_story_wav(data: bytes, maximum_bytes: int) -> tuple[dict[str, int], bytes]:
    sample_rate, pcm = _parse_story_wav(data)
    normalized = _build_story_wav(sample_rate, pcm)
    if len(normalized) > maximum_bytes:
        raise AiServiceError("TTS WAV size is outside the allowed range")
    frame_count = len(pcm) // 2
    return {
        "channels": 1,
        "sample_width": 2,
        "sample_rate": sample_rate,
        "frame_count": frame_count,
        "duration_ms": round(frame_count * 1000 / sample_rate),
    }, normalized


def validate_story_wav(data: bytes, maximum_bytes: int) -> dict[str, int]:
    info, _ = _normalize_story_wav(data, maximum_bytes)
    return info


def _split_story_text(text: str, maximum: int = STORY_TTS_CHUNK_CHARS) -> list[str]:
    sentences = re.findall(r".*?(?:[。！？!?；;]+|$)", text.strip(), flags=re.DOTALL)
    chunks: list[str] = []
    current = ""
    for sentence in (item.strip() for item in sentences if item.strip()):
        while len(sentence) > maximum:
            if current:
                chunks.append(current)
                current = ""
            chunks.append(sentence[:maximum])
            sentence = sentence[maximum:]
        if current and len(current) + len(sentence) > maximum:
            chunks.append(current)
            current = sentence
        else:
            current += sentence
    if current:
        chunks.append(current)
    return chunks or [text.strip()]


def _story_text_length(text: str) -> int:
    return len(re.sub(r"\s+", "", text))


def _merge_story_wavs(parts: list[bytes], maximum_bytes: int) -> tuple[bytes, dict[str, int]]:
    if not parts:
        raise AiServiceError("TTS returned no story audio")
    source_rate = 0
    pcm_parts: list[bytes] = []
    for part in parts:
        rate, pcm = _parse_story_wav(part)
        if source_rate and rate != source_rate:
            raise AiServiceError("TTS returned inconsistent sample rates")
        source_rate = rate
        if pcm_parts:
            pcm_parts.append(b"\0\0" * (source_rate * 60 // 1000))
        pcm_parts.append(pcm)
    pcm = b"".join(pcm_parts)
    if source_rate == 16000 and len(pcm) + 44 > min(
        maximum_bytes, STORY_PREFER_8K_BYTES
    ):
        pcm = _downsample_16k_to_8k(pcm)
        source_rate = 8000
    audio = _build_story_wav(source_rate, pcm)
    info, normalized = _normalize_story_wav(audio, maximum_bytes)
    return normalized, info


class QwenClient:
    def __init__(self, settings: Settings, client: httpx.AsyncClient | None = None) -> None:
        self.settings = settings
        self._owned_client = client is None
        self.client = client or httpx.AsyncClient(
            timeout=httpx.Timeout(90.0, connect=10.0),
            follow_redirects=True,
        )

    async def close(self) -> None:
        if self._owned_client:
            await self.client.aclose()

    def _headers(self) -> dict[str, str]:
        return {
            "Authorization": f"Bearer {self.settings.dashscope_api_key}",
            "Content-Type": "application/json",
        }

    async def chat_json(
        self,
        system_prompt: str,
        user_prompt: str,
        *,
        max_tokens: int,
        temperature: float,
    ) -> dict[str, Any]:
        response = await self.client.post(
            self.settings.qwen_chat_url,
            headers=self._headers(),
            json={
                "model": self.settings.qwen_model,
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt},
                ],
                "response_format": {"type": "json_object"},
                "max_tokens": max_tokens,
                "temperature": temperature,
            },
        )
        if response.status_code >= 400:
            raise AiServiceError(f"Qwen chat request failed with HTTP {response.status_code}")
        try:
            payload = response.json()
            content = payload["choices"][0]["message"]["content"]
        except (ValueError, KeyError, IndexError, TypeError) as error:
            raise AiServiceError("Qwen chat response is missing message content") from error
        return _parse_json_object(content)

    async def synthesize_wav(self, text: str) -> bytes:
        response = await self.client.post(
            self.settings.qwen_tts_url,
            headers=self._headers(),
            json={
                "model": self.settings.qwen_tts_model,
                "input": {
                    "text": text,
                    "voice": self.settings.qwen_tts_voice,
                    "format": "wav",
                    "sample_rate": 16000,
                    "rate": 0.92,
                    "volume": 60,
                },
            },
        )
        if response.status_code >= 400:
            raise AiServiceError(f"Qwen TTS request failed with HTTP {response.status_code}")
        try:
            payload = response.json()
        except ValueError as error:
            raise AiServiceError("Qwen TTS response is not JSON") from error
        output = payload.get("output") if isinstance(payload, dict) else None
        audio = output.get("audio") if isinstance(output, dict) else None
        url = audio.get("url") if isinstance(audio, dict) else None
        if not url and isinstance(output, dict):
            url = output.get("url") or output.get("audio_url")
        if not isinstance(url, str) or not url.startswith(("https://", "http://")):
            raise AiServiceError("Qwen TTS response is missing the audio URL")
        audio_response = await self.client.get(url)
        if audio_response.status_code >= 400:
            raise AiServiceError(f"TTS audio download failed with HTTP {audio_response.status_code}")
        return bytes(audio_response.content)


def _nested(mapping: dict[str, Any], *keys: str) -> Any:
    current: Any = mapping
    for key in keys:
        if not isinstance(current, dict):
            return None
        current = current.get(key)
    return current


def _integer(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _number(value: Any) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if result == result else None


def _label(labels: tuple[str, ...], value: Any, fallback: str = "unknown") -> str:
    index = _integer(value, -1)
    return labels[index] if 0 <= index < len(labels) else fallback


def _vision_scores(values: Any) -> dict[str, dict[str, float | int]]:
    scores = values if isinstance(values, list) else []
    return {
        name: {
            "score_x1000": _integer(scores[index]) if index < len(scores) else 0,
            "confidence": max(0.0, min(1.0, _integer(scores[index]) / 1000.0))
            if index < len(scores) else 0.0,
        }
        for index, name in enumerate(VISION_LABELS)
    }


def _vision_detections(values: Any) -> list[dict[str, Any]]:
    detections: list[dict[str, Any]] = []
    for raw in values[:8] if isinstance(values, list) else []:
        if not isinstance(raw, list) or len(raw) < 6:
            continue
        class_id = _integer(raw[0], -1)
        score = _integer(raw[1])
        detections.append({
            "class_id": class_id,
            "class_name": _label(VISION_LABELS, class_id),
            "score_x1000": score,
            "confidence": max(0.0, min(1.0, score / 1000.0)),
            "box": {
                "x": _integer(raw[2]), "y": _integer(raw[3]),
                "width": _integer(raw[4]), "height": _integer(raw[5]),
            },
        })
    return detections


def _compact_telemetry(payload: dict[str, Any]) -> dict[str, Any]:
    sensors = payload.get("sensors") if isinstance(payload.get("sensors"), dict) else {}
    inference = payload.get("inference") if isinstance(payload.get("inference"), dict) else {}
    vision = payload.get("vision") if isinstance(payload.get("vision"), dict) else {}
    health = payload.get("health") if isinstance(payload.get("health"), dict) else {}
    alarm = inference.get("alarm") if isinstance(inference.get("alarm"), list) else []
    fusion = dict(inference.get("fusion")) if isinstance(inference.get("fusion"), dict) else {
        "state": inference.get("state"),
        "candidate": inference.get("candidate"),
        "confidence": inference.get("conf"),
        "severity": inference.get("severity"),
        "sources": inference.get("sources"),
        "valid_mask": inference.get("valid"),
        "fault_mask": inference.get("faults"),
        "alarm_level": alarm[0] if alarm else 0,
        "alarm_type": alarm[1] if len(alarm) > 1 else 0,
    }
    comfort_raw = inference.get("comfort") if isinstance(inference.get("comfort"), list) else []
    comfort_values = [_integer(value) for value in comfort_raw[:5]]
    comfort_values.extend([0] * (5 - len(comfort_values)))
    comfort_actions, comfort_reasons, comfort_confidence, comfort_confirmation, comfort_blocks = comfort_values
    fusion["comfort"] = {
        "available": len(comfort_raw) >= 5,
        "action_mask": comfort_actions,
        "reason_mask": comfort_reasons,
        "confidence": max(0, min(100, comfort_confidence)),
        "requires_user_confirmation": bool(comfort_confirmation),
        "block_mask": comfort_blocks,
        "recommended_light": "on" if comfort_actions & (1 << 1) else (
            "off" if comfort_actions & (1 << 0) else None
        ),
        "recommend_pat": bool(comfort_actions & (1 << 2)),
        "caregiver_check": bool(comfort_actions & (1 << 3)),
    }
    safe = inference.get("safe") if isinstance(inference.get("safe"), dict) else {
        "level": alarm[0] if alarm else 0,
        "severity": inference.get("severity"),
    }
    thermal = sensors.get("thermal") if isinstance(sensors.get("thermal"), dict) else {}
    pressure = sensors.get("pressure") if isinstance(sensors.get("pressure"), dict) else {}
    temperature = sensors.get("bed_temperature") if isinstance(sensors.get("bed_temperature"), dict) else {}
    microphone = sensors.get("microphone") if isinstance(sensors.get("microphone"), dict) else {}
    environment = sensors.get("environment") if isinstance(sensors.get("environment"), dict) else {}
    vision_status = _integer(vision.get("s"), -1)
    thermal_quality = _integer(thermal.get("frame_quality"), -1)
    return {
        "sequence": int(payload.get("sequence") or 0),
        "ra8_tick": int(payload.get("ra8_tick") or 0),
        "fusion": fusion,
        "safety": safe,
        "vision": {
            "status": vision_status,
            "status_label": _label(VISION_STATUS_LABELS, vision_status),
            "frame_sequence": _integer(vision.get("q")),
            "models_ready_mask": _integer(vision.get("r")),
            "fusion_eligible": bool(_integer(vision.get("fe"))),
            "class_scores": _vision_scores(vision.get("ss")),
            "detections": _vision_detections(vision.get("d")),
        },
        "cry": {
            "class": microphone.get("cls", microphone.get("cry")),
            "confidence": microphone.get("conf", microphone.get("cry_conf")),
            "peak": microphone.get("peak"),
            "rms": microphone.get("rms", microphone.get("mean_abs")),
        },
        "pressure": {
            "status": pressure.get("s"),
            "total": pressure.get("total", pressure.get("sum")),
            "contacts": pressure.get("contacts", pressure.get("nz")),
            "edge": pressure.get("edge"),
            "center_x": pressure.get("cx"),
            "center_y": pressure.get("cy"),
        },
        "thermal": {
            "status": thermal.get("s"),
            "frame_quality": thermal_quality,
            "frame_quality_label": _label(THERMAL_QUALITY_LABELS, thermal_quality),
            "minimum_cdeg": thermal.get("p01_cd", thermal.get("min_cd")),
            "maximum_cdeg": thermal.get("p99_cd", thermal.get("max_cd")),
            "mean_cdeg": thermal.get("mean_cd", thermal.get("avg_cd")),
            "valid_points": thermal.get("valid"),
        },
        "bed_temperature_milli_c": temperature.get("t_mc", []),
        "environment": environment,
        "environment_policy": payload.get("environment_policy", {}),
        "actuators": {
            "warm_light": sensors.get("warm_light", {}),
            "soother": sensors.get("soother", {}),
        },
        "health": {
            "sd_ready": health.get("sd_ready", _nested(microphone, "sdr")),
            "gateway": health.get("gateway"),
            "sensor_fault_mask": health.get("sensor_fault_mask", inference.get("faults")),
        },
    }


def _deterministic_findings(summary: dict[str, Any]) -> list[dict[str, str]]:
    """Evidence that may raise advisory level without model interpretation."""
    findings: list[dict[str, str]] = []
    fusion = summary.get("fusion") if isinstance(summary.get("fusion"), dict) else {}
    safety = summary.get("safety") if isinstance(summary.get("safety"), dict) else {}
    event = summary.get("latest_local_event") if isinstance(summary.get("latest_local_event"), dict) else {}
    local_values = (
        fusion.get("level"), fusion.get("alarm_level"), fusion.get("severity"), safety.get("level"),
        safety.get("severity"), event.get("severity"),
    )
    local_max = max((_number(value) or 0.0 for value in local_values), default=0.0)
    if local_max > 0:
        findings.append({
            "level": "urgent" if local_max >= 3 else "attention",
            "category": "local_safety",
            "evidence": f"RA8 本地融合或安全层等级为 {local_max:g}",
        })

    health = summary.get("health") if isinstance(summary.get("health"), dict) else {}
    fault_mask = _integer(health.get("sensor_fault_mask"))
    if fault_mask:
        findings.append({
            "level": "attention", "category": "sensor",
            "evidence": f"RA8 传感器故障位掩码为 0x{fault_mask:X}",
        })

    vision = summary.get("vision") if isinstance(summary.get("vision"), dict) else {}
    vision_status = _integer(vision.get("status"), -1)
    if vision_status >= 7:
        findings.append({
            "level": "attention", "category": "vision",
            "evidence": f"视觉运行状态为 {vision.get('status_label', vision_status)}",
        })

    thermal = summary.get("thermal") if isinstance(summary.get("thermal"), dict) else {}
    quality = _integer(thermal.get("frame_quality"), -1)
    valid_points = _integer(thermal.get("valid_points"), -1)
    if quality == 2 or (valid_points >= 0 and valid_points != 5120):
        findings.append({
            "level": "attention", "category": "thermal",
            "evidence": f"热成像质量={thermal.get('frame_quality_label', quality)}，有效点={valid_points}",
        })

    environment_policy = (
        summary.get("environment_policy")
        if isinstance(summary.get("environment_policy"), dict)
        else {}
    )
    if environment_policy.get("active"):
        names = {
            "co2": "CO2",
            "temperature": "温度",
            "humidity": "湿度",
            "gas_resistance": "BME688 原始气阻",
            "body_temperature": "婴儿体表温度筛查值",
        }
        for item in environment_policy.get("exceeded", []):
            if not isinstance(item, dict):
                continue
            direction = "高于" if item.get("direction") == "high" else "低于"
            item_name = str(item.get("name"))
            findings.append({
                "level": "attention",
                "category": "body_temperature" if item_name == "body_temperature" else "environment",
                "evidence": (
                    f"婴儿在床时 {names.get(item_name, item.get('name'))} "
                    f"{item.get('value')}，{direction}门限 {item.get('threshold')}"
                ),
            })
    elif not environment_policy:
        # Compatibility for stored telemetry produced before configurable policy support.
        environment = summary.get("environment") if isinstance(summary.get("environment"), dict) else {}
        co2 = _number(environment.get("co2", environment.get("co2_ppm")))
        if co2 is not None and co2 >= 1500:
            findings.append({
                "level": "attention", "category": "environment",
                "evidence": f"环境 CO2 为 {co2:.0f} ppm，高于 1500 ppm 关注阈值",
            })
    return findings


def _telemetry_requires_attention(summary: dict[str, Any]) -> bool:
    return bool(_deterministic_findings(summary))


def _local_required_level(summary: dict[str, Any]) -> str:
    """Return the minimum advisory level required by deterministic RA8 evidence."""
    levels = {finding["level"] for finding in _deterministic_findings(summary)}
    if "urgent" in levels:
        return "urgent"
    if "attention" in levels:
        return "attention"
    return "normal"


def _deterministic_advisory(
    findings: list[dict[str, str]], level: str
) -> tuple[str, str, list[str], list[str]]:
    """Build user-facing text without letting the model reinterpret hard evidence."""
    categories = {finding["category"] for finding in findings}
    evidence = [finding["evidence"] for finding in findings]
    actions: list[str] = []

    if "local_safety" in categories:
        actions.append("立即查看婴儿状态、实时画面和 RA8 本地告警详情")
    if "sensor" in categories:
        actions.append("检查页面标出的故障传感器、接线和供电")
    if "vision" in categories:
        actions.append("检查摄像头画面、镜头遮挡和视觉推理状态")
    if "thermal" in categories:
        actions.append("检查热成像模组供电、连接和帧质量诊断")
    if "environment" in categories:
        actions.append("检查房间通风并复核环境传感器读数")
    if "body_temperature" in categories:
        actions.append("尽快使用可靠体温计复测；热成像筛查值不能代替医疗体温测量")

    if categories == {"environment"}:
        return (
            "环境数据需要关注",
            "环境传感器出现明确越限，建议优先改善婴儿床周围空气条件。",
            evidence,
            actions,
        )

    if categories == {"body_temperature"}:
        return (
            "疑似发热需要复测",
            "热成像体表温度筛查值持续越限，请尽快使用可靠体温计复测。",
            evidence,
            actions,
        )

    if "local_safety" in categories:
        title = "RA8 本地安全告警" if level == "urgent" else "RA8 本地安全状态需要关注"
        summary = "RA8 本地融合或安全层已报告明确状态，请以本地硬件告警和实时传感器为准。"
    else:
        title = "监护链路需要检查"
        summary = "一个或多个监护数据源存在明确异常，当前结果可能不完整。"

    if "environment" in categories:
        summary += " 同时检测到环境数据越限。"
    if "body_temperature" in categories:
        summary += " 同时检测到体表温度筛查值越限，需要用体温计复测。"
    return title, summary, evidence, actions


def _story_view(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "story_id": row["story_id"],
        "device_id": row["device_id"],
        "request_id": row.get("request_id"),
        "status": row["status"],
        "request": _loads(row.get("request_json"), {}),
        "title": row.get("title", ""),
        "story_text": row.get("story_text", ""),
        "model": row.get("model", ""),
        "tts_model": row.get("tts_model", ""),
        "audio_size_bytes": int(row.get("audio_size_bytes") or 0),
        "audio_duration_ms": int(row.get("audio_duration_ms") or 0),
        "command_id": row.get("command_id"),
        "error": row.get("error", ""),
        "created_at": row.get("created_at"),
        "updated_at": row.get("updated_at"),
    }


def _advisory_view(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "advisory_id": row["advisory_id"],
        "device_id": row["device_id"],
        "level": row["level"],
        "title": row["title"],
        "summary": row["summary"],
        "evidence": _loads(row.get("evidence_json"), []),
        "actions": _loads(row.get("actions_json"), []),
        "confidence": float(row.get("confidence") or 0.0),
        "requires_user_attention": bool(row.get("requires_user_attention")),
        "source_sequence": int(row.get("source_sequence") or 0),
        "model": row.get("model", ""),
        "acknowledged": bool(row.get("acknowledged")),
        "created_at": row.get("created_at"),
    }


@dataclass(frozen=True)
class _StoryJob:
    story_id: str
    device_id: str


class AiRuntime:
    def __init__(
        self,
        settings: Settings,
        database: Database,
        publish: PublishCallback,
        *,
        client: QwenClient | None = None,
    ) -> None:
        self.settings = settings
        self.database = database
        self.publish = publish
        self.client = client or QwenClient(settings)
        self.story_dir = settings.data_dir / "stories"
        self._story_queue: asyncio.Queue[_StoryJob] = asyncio.Queue(maxsize=STORY_QUEUE_LIMIT)
        self._assessment_queue: asyncio.Queue[str] = asyncio.Queue(maxsize=ASSESSMENT_QUEUE_LIMIT)
        self._story_create_lock = asyncio.Lock()
        self._latest_telemetry: dict[str, dict[str, Any]] = {}
        self._pending_assessments: set[str] = set()
        self._force_assessments: set[str] = set()
        self._last_assessment: dict[str, float] = {}
        self._last_attention_assessment: dict[str, float] = {}
        self._tasks: list[asyncio.Task[None]] = []
        self._closed = False

    @property
    def enabled(self) -> bool:
        return self.settings.ai_enabled and bool(self.settings.dashscope_api_key)

    async def start(self) -> None:
        self.story_dir.mkdir(parents=True, exist_ok=True)
        if self.enabled and not self._tasks:
            self._tasks = [
                asyncio.create_task(self._story_worker(), name="ai-story-worker"),
                asyncio.create_task(self._assessment_worker(), name="ai-assessment-worker"),
            ]

    async def close(self) -> None:
        self._closed = True
        for task in self._tasks:
            task.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()
        await self.client.close()

    def status(self) -> dict[str, Any]:
        return {
            "enabled": self.enabled,
            "model": self.settings.qwen_model,
            "tts_model": self.settings.qwen_tts_model,
            "story_queue_depth": self._story_queue.qsize(),
            "story_queue_limit": STORY_QUEUE_LIMIT,
            "assessment_queue_depth": self._assessment_queue.qsize(),
            "assessment_queue_limit": ASSESSMENT_QUEUE_LIMIT,
        }

    async def create_story(self, device_id: str, request: dict[str, Any]) -> tuple[dict[str, Any], bool]:
        if not self.enabled:
            raise AiServiceError("AI service is disabled")
        request_id = _clip_text(request.get("request_id"), 64) or None
        async with self._story_create_lock:
            if request_id:
                duplicate = await asyncio.to_thread(
                    self.database.query_one,
                    "SELECT * FROM ai_story_jobs WHERE device_id=? AND request_id=?",
                    (device_id, request_id),
                )
                if duplicate:
                    return _story_view(duplicate), True

            active = await asyncio.to_thread(
                self.database.query_one,
                "SELECT * FROM ai_story_jobs WHERE device_id=? "
                "AND status IN ('queued', 'generating', 'synthesizing', 'queued_for_device') "
                "ORDER BY created_at DESC LIMIT 1",
                (device_id,),
            )
            if active:
                return _story_view(active), True

            story_id = ""
            for _ in range(32):
                candidate = uuid.uuid4().hex
                filename_token = candidate[:6].upper()
                collision = await asyncio.to_thread(
                    self.database.query_one,
                    "SELECT story_id FROM ai_story_jobs WHERE upper(substr(story_id, 1, 6))=?",
                    (filename_token,),
                )
                if not collision:
                    story_id = candidate
                    break
            if not story_id:
                raise AiServiceError("unable to allocate a unique story filename")

            now = utc_now()
            await asyncio.to_thread(
                self.database.execute,
                "INSERT INTO ai_story_jobs(story_id, device_id, request_id, status, request_json, "
                "model, tts_model, created_at, updated_at) VALUES(?, ?, ?, 'queued', ?, ?, ?, ?, ?)",
                (
                    story_id,
                    device_id,
                    request_id,
                    _json(request),
                    self.settings.qwen_model,
                    self.settings.qwen_tts_model,
                    now,
                    now,
                ),
            )
        try:
            self._story_queue.put_nowait(_StoryJob(story_id, device_id))
        except asyncio.QueueFull as error:
            await asyncio.to_thread(
                self.database.execute,
                "UPDATE ai_story_jobs SET status='failed', error=?, updated_at=? WHERE story_id=?",
                ("story queue is busy", utc_now(), story_id),
            )
            raise AiServiceError("story queue is busy") from error
        row = await asyncio.to_thread(
            self.database.query_one, "SELECT * FROM ai_story_jobs WHERE story_id=?", (story_id,)
        )
        view = _story_view(row or {"story_id": story_id, "device_id": device_id, "status": "queued"})
        await self.publish("ai_story", device_id, view)
        return view, False

    async def replay_story(self, device_id: str, story_id: str) -> dict[str, Any]:
        row = await asyncio.to_thread(
            self.database.query_one,
            "SELECT * FROM ai_story_jobs WHERE story_id=? AND device_id=?",
            (story_id, device_id),
        )
        if not row or not row.get("audio_relative_path"):
            raise AiServiceError("story audio is not ready")
        command_id = await self._enqueue_story_command(row)
        await asyncio.to_thread(
            self.database.execute,
            "UPDATE ai_story_jobs SET status='queued_for_device', command_id=?, error='', updated_at=? "
            "WHERE story_id=?",
            (command_id, utc_now(), story_id),
        )
        updated = await asyncio.to_thread(
            self.database.query_one, "SELECT * FROM ai_story_jobs WHERE story_id=?", (story_id,)
        )
        view = _story_view(updated or row)
        await self.publish("ai_story", device_id, view)
        return view

    def offer_telemetry(self, device_id: str, payload: dict[str, Any]) -> None:
        if not self.enabled or self._closed:
            return
        summary = _compact_telemetry(payload)
        self._latest_telemetry[device_id] = summary
        now = time.monotonic()
        attention = _telemetry_requires_attention(summary)
        due = now - self._last_assessment.get(device_id, 0.0) >= self.settings.ai_assessment_interval_seconds
        urgent_due = (
            attention
            and now - self._last_attention_assessment.get(device_id, 0.0)
            >= self.settings.ai_event_cooldown_seconds
        )
        if not (due or urgent_due) or device_id in self._pending_assessments:
            return
        if urgent_due:
            self._force_assessments.add(device_id)
        self._queue_assessment(device_id)

    def offer_event(self, device_id: str, event: dict[str, Any]) -> None:
        if not self.enabled or self._closed:
            return
        summary = dict(self._latest_telemetry.get(device_id, {}))
        summary["latest_local_event"] = {
            key: event.get(key)
            for key in ("event_type", "state", "severity", "confidence", "sources")
        }
        self._latest_telemetry[device_id] = summary
        self._force_assessments.add(device_id)
        if device_id not in self._pending_assessments:
            self._queue_assessment(device_id)

    def request_assessment(
        self,
        device_id: str,
        telemetry: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        if not self.enabled:
            raise AiServiceError("AI service is disabled")
        if telemetry:
            self._latest_telemetry[device_id] = _compact_telemetry(telemetry)
        if not self._latest_telemetry.get(device_id):
            raise AiServiceError("device telemetry is not available")
        self._force_assessments.add(device_id)
        already_pending = device_id in self._pending_assessments
        if not already_pending:
            self._queue_assessment(device_id)
        return {
            "accepted": True,
            "already_pending": already_pending,
            "queue_depth": self._assessment_queue.qsize(),
        }

    def _queue_assessment(self, device_id: str) -> None:
        if self._assessment_queue.full():
            try:
                dropped = self._assessment_queue.get_nowait()
                self._assessment_queue.task_done()
                self._pending_assessments.discard(dropped)
            except asyncio.QueueEmpty:
                pass
        try:
            self._assessment_queue.put_nowait(device_id)
            self._pending_assessments.add(device_id)
        except asyncio.QueueFull:
            pass

    async def command_acked(self, device_id: str, command_id: int, ok: bool, result: dict[str, Any]) -> None:
        row = await asyncio.to_thread(
            self.database.query_one,
            "SELECT * FROM ai_story_jobs WHERE command_id=? AND device_id=?",
            (command_id, device_id),
        )
        if not row:
            return
        # A successful gateway ACK means the verified WAV is stored on RA8 and
        # queued for playback. Playback completion remains visible in
        # sensors.audio_player.story.s and is not inferred here.
        error = "" if ok else _clip_text(result.get("error") or result.get("message") or "device rejected story", 240)
        # A replay submitted while another transfer is active was not attempted.
        # Keep the verified WAV reusable instead of presenting this as a Qwen/TTS
        # generation failure; the user can replay it after the active transfer.
        transfer_busy = not ok and error == "another story transfer is in progress"
        status = "queued_for_playback" if ok else ("ready" if transfer_busy else "failed")
        if transfer_busy:
            error = ""
        await asyncio.to_thread(
            self.database.execute,
            "UPDATE ai_story_jobs SET status=?, error=?, updated_at=? WHERE story_id=?",
            (status, error, utc_now(), row["story_id"]),
        )
        updated = await asyncio.to_thread(
            self.database.query_one,
            "SELECT * FROM ai_story_jobs WHERE story_id=?",
            (row["story_id"],),
        )
        await self.publish("ai_story", device_id, _story_view(updated or row))

    async def _story_worker(self) -> None:
        while True:
            job = await self._story_queue.get()
            try:
                await self._process_story(job)
            except asyncio.CancelledError:
                raise
            except Exception as error:
                message = _clip_text(error, 240) or "story generation failed"
                await asyncio.to_thread(
                    self.database.execute,
                    "UPDATE ai_story_jobs SET status='failed', error=?, updated_at=? WHERE story_id=?",
                    (message, utc_now(), job.story_id),
                )
                row = await asyncio.to_thread(
                    self.database.query_one,
                    "SELECT * FROM ai_story_jobs WHERE story_id=?",
                    (job.story_id,),
                )
                if row:
                    await self.publish("ai_story", job.device_id, _story_view(row))
            finally:
                self._story_queue.task_done()

    async def _process_story(self, job: _StoryJob) -> None:
        row = await asyncio.to_thread(
            self.database.query_one, "SELECT * FROM ai_story_jobs WHERE story_id=?", (job.story_id,)
        )
        if not row:
            return
        request = _loads(row.get("request_json"), {})
        await self._update_story_status(job, "generating")
        target_seconds = max(30, min(240, int(request.get("duration_seconds") or 60)))
        target_chars = max(140, min(1050, round(target_seconds * STORY_CHARS_PER_SECOND)))
        minimum_chars = round(target_chars * 0.88)
        maximum_chars = round(target_chars * 1.08)
        story_system_prompt = (
            "你是婴幼儿睡前故事作者。只输出JSON，不得包含Markdown。故事必须有明确主人公、具体目标、"
            "一个温和的小阻碍、主人公为解决问题采取的至少两次行动、一个自然转折和完整结局；事件之间必须有因果关系。"
            "环境描写不得超过正文的20%，禁止连续堆砌月光、星星、云朵、微风、花香等氛围词，禁止只有散文式意象而没有情节。"
            "内容必须温和、舒缓、无惊吓、"
            "无医疗结论、无危险动作模仿，不要求婴儿作出回应。JSON字段必须为title、story_text、"
            "age_range、estimated_seconds、safety_notes。story_text 必须是一篇完整连贯的故事。"
        )
        story_request = {
                "task": "生成中文睡前故事",
                "theme": _clip_text(request.get("theme"), 120),
                "child_age_months": int(request.get("child_age_months") or 12),
                "target_seconds": target_seconds,
                "target_chinese_characters": target_chars,
                "style": request.get("style", "calm"),
                "constraints": (
                    f"JSON输出；正文必须为{minimum_chars}至{maximum_chars}个汉字；"
                    "使用多个自然段；按开端、阻碍、行动、转折、解决、安静收尾展开完整情节；结尾自然、安静"
                ),
            }
        generated = await self.client.chat_json(
            story_system_prompt,
            _json(story_request),
            max_tokens=max(900, min(2600, target_chars * 2 + 300)),
            temperature=0.75,
        )
        title = _clip_text(generated.get("title"), 80)
        story_text = _clip_text(generated.get("story_text"), 1800)
        if _story_text_length(story_text) < minimum_chars:
            expanded = await self.client.chat_json(
                story_system_prompt,
                _json({
                    "task": "扩写为完整中文睡前故事，返回完整替换稿而不是续写片段",
                    "original_title": title,
                    "original_story": story_text,
                    "theme": story_request["theme"],
                    "target_seconds": target_seconds,
                    "target_chinese_characters": target_chars,
                    "constraints": story_request["constraints"],
                }),
                max_tokens=max(900, min(2600, target_chars * 2 + 300)),
                temperature=0.65,
            )
            expanded_text = _clip_text(expanded.get("story_text"), 1800)
            if _story_text_length(expanded_text) > _story_text_length(story_text):
                title = _clip_text(expanded.get("title"), 80) or title
                story_text = expanded_text
        for continuation_index in range(1, 4):
            current_chars = _story_text_length(story_text)
            if current_chars >= minimum_chars:
                break
            remaining_chars = max(100, target_chars - current_chars)
            continuation_chars = min(420, remaining_chars + 40)
            continuation = await self.client.chat_json(
                story_system_prompt,
                _json({
                    "task": "只续写前文的新段落，不得复述或从头重写",
                    "title": title,
                    "theme": story_request["theme"],
                    "previous_story": story_text[-1200:],
                    "continuation_index": continuation_index,
                    "target_new_chinese_characters": continuation_chars,
                    "constraints": (
                        f"只在story_text返回约{continuation_chars}个汉字的新续写；"
                        "承接前文情节；不得重复已有句子；最后一段自然安静地结束"
                    ),
                }),
                max_tokens=max(700, min(1400, continuation_chars * 2 + 250)),
                temperature=0.65,
            )
            new_text = _clip_text(continuation.get("story_text"), 900)
            if not new_text or new_text in story_text:
                break
            # Some models ignore the continuation instruction and return a
            # complete replacement. Accept it only when it really adds length.
            if new_text[:80] == story_text[:80]:
                if _story_text_length(new_text) <= current_chars:
                    break
                story_text = new_text
            else:
                story_text = _clip_text(f"{story_text}\n{new_text}", 1800)
        if not title or len(story_text) < 20:
            raise AiServiceError("generated story is incomplete")
        await asyncio.to_thread(
            self.database.execute,
            "UPDATE ai_story_jobs SET status='synthesizing', title=?, story_text=?, updated_at=? "
            "WHERE story_id=?",
            (title, story_text, utc_now(), job.story_id),
        )
        tts_parts = []
        for chunk in _split_story_text(story_text):
            tts_parts.append(await self.client.synthesize_wav(chunk))
        audio, wav_info = _merge_story_wavs(tts_parts, self.settings.ai_story_max_bytes)
        filename = f"AI{job.story_id[:6].upper()}.WAV"
        if not STORY_FILENAME.fullmatch(filename):
            raise AiServiceError("internal story filename validation failed")
        target = self.story_dir / filename
        temporary = target.with_suffix(".TMP")
        await asyncio.to_thread(temporary.write_bytes, audio)
        await asyncio.to_thread(temporary.replace, target)
        sha256 = hashlib.sha256(audio).hexdigest()
        crc32 = binascii.crc32(audio) & 0xFFFFFFFF
        relative_path = target.relative_to(self.settings.data_dir).as_posix()
        await asyncio.to_thread(
            self.database.execute,
            "UPDATE ai_story_jobs SET status='ready', audio_relative_path=?, audio_sha256=?, "
            "audio_crc32=?, audio_size_bytes=?, audio_duration_ms=?, updated_at=? WHERE story_id=?",
            (
                relative_path,
                sha256,
                crc32,
                len(audio),
                wav_info["duration_ms"],
                utc_now(),
                job.story_id,
            ),
        )
        row = await asyncio.to_thread(
            self.database.query_one, "SELECT * FROM ai_story_jobs WHERE story_id=?", (job.story_id,)
        )
        if row and bool(request.get("autoplay", True)):
            command_id = await self._enqueue_story_command(row)
            await asyncio.to_thread(
                self.database.execute,
                "UPDATE ai_story_jobs SET status='queued_for_device', command_id=?, updated_at=? "
                "WHERE story_id=?",
                (command_id, utc_now(), job.story_id),
            )
            row = await asyncio.to_thread(
                self.database.query_one, "SELECT * FROM ai_story_jobs WHERE story_id=?", (job.story_id,)
            )
        if row:
            await self.publish("ai_story", job.device_id, _story_view(row))

    async def _update_story_status(self, job: _StoryJob, status: str) -> None:
        await asyncio.to_thread(
            self.database.execute,
            "UPDATE ai_story_jobs SET status=?, error='', updated_at=? WHERE story_id=?",
            (status, utc_now(), job.story_id),
        )
        row = await asyncio.to_thread(
            self.database.query_one, "SELECT * FROM ai_story_jobs WHERE story_id=?", (job.story_id,)
        )
        if row:
            await self.publish("ai_story", job.device_id, _story_view(row))

    async def _enqueue_story_command(self, row: dict[str, Any]) -> int:
        filename = Path(str(row["audio_relative_path"])).name
        command = {
            "command": "story.audio.play",
            "args": {
                "story_id": row["story_id"],
                "path": f"/api/v1/ai/stories/{row['story_id']}/audio",
                "filename": filename,
                "size_bytes": int(row["audio_size_bytes"]),
                "sha256": row["audio_sha256"],
                "crc32": int(row["audio_crc32"]),
                "autoplay": True,
            },
        }
        command_row, _ = await asyncio.to_thread(
            self.database.enqueue_command,
            row["device_id"],
            _json(command),
            utc_now(),
            f"story:{row['story_id']}:{uuid.uuid4().hex[:8]}",
        )
        command_id = int(command_row["id"])
        await self.publish(
            "command",
            row["device_id"],
            {"id": command_id, "status": "queued", "action": "story.audio.play"},
        )
        return command_id

    async def _assessment_worker(self) -> None:
        while True:
            device_id = await self._assessment_queue.get()
            self._pending_assessments.discard(device_id)
            try:
                summary = dict(self._latest_telemetry.get(device_id, {}))
                if summary:
                    await self._assess(device_id, summary)
            except asyncio.CancelledError:
                raise
            except Exception:
                # Assessment is advisory only. Failures must never affect device ACKs.
                pass
            finally:
                self._assessment_queue.task_done()

    async def _assess(self, device_id: str, summary: dict[str, Any]) -> None:
        now_mono = time.monotonic()
        forced = device_id in self._force_assessments
        self._force_assessments.discard(device_id)
        result = await self.client.chat_json(
            "你是婴儿监护系统的解释层，不是医疗诊断系统。底层本地规则和硬件告警优先，"
            "你只能综合已给出的结构化遥测做简短解释，不能臆测缺失数据。只输出JSON，字段必须为"
            "level(normal/attention/urgent)、title、summary、evidence(字符串数组)、actions(字符串数组)、"
            "confidence(0到1)、requires_user_attention(布尔)。urgent只能用于已有明确高风险证据。"
            "vision.class_scores 是命名后的类别置信度而不是故障码；blanket 是婴儿床正常物品，单独"
            "检测到毛毯绝不是风险。vision.status=6/ready 和 thermal.frame_quality=0/normal 都表示正常。"
            "只有 RA8 本地融合/安全事件、明确传感器故障或环境越限才允许输出 attention/urgent。"
            "fusion.comfort 是 RA8 给出的安抚动作建议，必须由用户确认；不得声称暖光灯或拍一拍已经自动执行，"
            "实际执行状态只能依据 actuators 中的设备回传。",
            _json({"task": "综合评估婴儿状态", "telemetry": summary}),
            max_tokens=600,
            temperature=0.15,
        )
        level = str(result.get("level", "normal")).lower()
        if level not in {"normal", "attention", "urgent"}:
            level = "attention"
        findings = _deterministic_findings(summary)
        minimum_level = _local_required_level(summary)
        level_rank = {"normal": 0, "attention": 1, "urgent": 2}
        if level_rank[level] < level_rank[minimum_level]:
            level = minimum_level
        elif level == "urgent" and minimum_level != "urgent":
            # The model explains deterministic RA8 evidence; it does not create
            # a new emergency level on its own. Subtle concerns remain visible
            # as attention advisories without becoming a hardware alarm.
            level = "attention"
        title = _clip_text(result.get("title"), 80) or "婴儿状态评估"
        text = _clip_text(result.get("summary"), 300)
        raw_evidence = result.get("evidence", [])
        raw_actions = result.get("actions", [])
        evidence = [
            _clip_text(item, 160)
            for item in (raw_evidence[:8] if isinstance(raw_evidence, list) else [])
        ]
        actions = [
            _clip_text(item, 160)
            for item in (raw_actions[:8] if isinstance(raw_actions, list) else [])
        ]
        deterministic_evidence = [finding["evidence"] for finding in findings]
        if minimum_level == "normal" and level != "normal":
            level = "normal"
            title = "当前未发现确定性风险"
            text = "模型提出的关注项缺少 RA8 本地融合、环境越限或明确传感器故障证据，未升级为告警。"
            evidence = ["毛毯或单个视觉类别置信度本身不构成风险"]
            actions = ["继续观察本地融合与传感器状态"]
        elif deterministic_evidence:
            title, text, evidence, actions = _deterministic_advisory(findings, level)
        try:
            confidence = max(0.0, min(1.0, float(result.get("confidence", 0.0))))
        except (TypeError, ValueError):
            confidence = 0.0
        requires_attention = level != "normal"
        fingerprint_source = dict(summary)
        fingerprint_source.pop("sequence", None)
        fingerprint_source.pop("ra8_tick", None)
        fingerprint = hashlib.sha256(_json(fingerprint_source).encode("utf-8")).hexdigest()
        latest = await asyncio.to_thread(
            self.database.query_one,
            "SELECT source_fingerprint, level FROM ai_advisories WHERE device_id=? "
            "ORDER BY created_at DESC LIMIT 1",
            (device_id,),
        )
        if latest and latest.get("source_fingerprint") == fingerprint and latest.get("level") == level and not forced:
            self._last_assessment[device_id] = now_mono
            return
        advisory_id = uuid.uuid4().hex
        created_at = utc_now()
        await asyncio.to_thread(
            self.database.execute,
            "INSERT INTO ai_advisories(advisory_id, device_id, level, title, summary, evidence_json, "
            "actions_json, confidence, requires_user_attention, source_sequence, source_fingerprint, "
            "model, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                advisory_id,
                device_id,
                level,
                title,
                text,
                _json(evidence),
                _json(actions),
                confidence,
                int(requires_attention),
                int(summary.get("sequence") or 0),
                fingerprint,
                self.settings.qwen_model,
                created_at,
            ),
        )
        row = await asyncio.to_thread(
            self.database.query_one,
            "SELECT * FROM ai_advisories WHERE advisory_id=?",
            (advisory_id,),
        )
        self._last_assessment[device_id] = now_mono
        if requires_attention:
            self._last_attention_assessment[device_id] = now_mono
        if row:
            await self.publish("ai_advisory", device_id, _advisory_view(row))

    async def list_stories(self, device_id: str, limit: int) -> list[dict[str, Any]]:
        rows = await asyncio.to_thread(
            self.database.query_all,
            "SELECT * FROM ai_story_jobs WHERE device_id=? ORDER BY created_at DESC LIMIT ?",
            (device_id, limit),
        )
        return [_story_view(row) for row in rows]

    async def get_story(self, device_id: str, story_id: str) -> dict[str, Any] | None:
        row = await asyncio.to_thread(
            self.database.query_one,
            "SELECT * FROM ai_story_jobs WHERE device_id=? AND story_id=?",
            (device_id, story_id),
        )
        return _story_view(row) if row else None

    async def get_story_audio(self, device_id: str, story_id: str) -> tuple[Path, dict[str, Any]]:
        row = await asyncio.to_thread(
            self.database.query_one,
            "SELECT * FROM ai_story_jobs WHERE device_id=? AND story_id=?",
            (device_id, story_id),
        )
        if not row or not row.get("audio_relative_path"):
            raise AiServiceError("story audio is not ready")
        path = (self.settings.data_dir / str(row["audio_relative_path"])).resolve()
        root = self.story_dir.resolve()
        if root not in path.parents or not path.is_file():
            raise AiServiceError("story audio file is unavailable")
        return path, row

    async def list_advisories(self, device_id: str, limit: int) -> list[dict[str, Any]]:
        rows = await asyncio.to_thread(
            self.database.query_all,
            "SELECT * FROM ai_advisories WHERE device_id=? ORDER BY created_at DESC LIMIT ?",
            (device_id, limit),
        )
        return [_advisory_view(row) for row in rows]

    async def acknowledge_advisory(self, device_id: str, advisory_id: str) -> bool:
        row = await asyncio.to_thread(
            self.database.query_one,
            "SELECT advisory_id FROM ai_advisories WHERE device_id=? AND advisory_id=?",
            (device_id, advisory_id),
        )
        if not row:
            return False
        await asyncio.to_thread(
            self.database.execute,
            "UPDATE ai_advisories SET acknowledged=1 WHERE advisory_id=?",
            (advisory_id,),
        )
        return True
