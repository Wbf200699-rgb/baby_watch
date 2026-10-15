from __future__ import annotations

import io
import json
import struct
import sys
import uuid
import wave
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import IsolatedAsyncioTestCase
from unittest.mock import patch

import httpx

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.ai_service import (
    AiRuntime,
    AiServiceError,
    QwenClient,
    _compact_telemetry,
    _local_required_level,
    _merge_story_wavs,
    _normalize_story_wav,
    validate_story_wav,
)
from app.config import Settings
from app.db import Database


def pcm_wav(*, channels: int = 1, sample_rate: int = 16000, frames: int = 1600) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(struct.pack(f"<{frames * channels}h", *([300] * frames * channels)))
    return output.getvalue()


class FakeQwenClient:
    def __init__(self) -> None:
        self.closed = False

    async def close(self) -> None:
        self.closed = True

    async def chat_json(self, system_prompt: str, user_prompt: str, **_: object) -> dict[str, object]:
        if "睡前故事作者" in system_prompt:
            return {
                "title": "月亮的小毯子",
                "story_text": "小月亮把柔软的银光铺在窗边，小云朵安静地盖好毯子，慢慢进入甜甜的梦乡。",
                "age_range": "0-3岁",
                "estimated_seconds": 45,
                "safety_notes": [],
            }
        return {
            "level": "attention",
            "title": "请查看婴儿状态",
            "summary": "本地安全层报告需要关注。",
            "evidence": ["安全等级非零"],
            "actions": ["查看实时画面"],
            "confidence": 0.8,
            "requires_user_attention": True,
        }

    async def synthesize_wav(self, _: str) -> bytes:
        return pcm_wav()


class AiRuntimeTest(IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temp = TemporaryDirectory()
        data = Path(self.temp.name)
        self.settings = Settings(
            root_dir=data,
            data_dir=data,
            database_path=data / "db.sqlite3",
            artifact_dir=data / "artifacts",
            device_keys={"dev": "secret"},
            ai_enabled=True,
            dashscope_api_key="test-only-key",
        )
        self.database = Database(self.settings.database_path)
        self.database.initialize()
        self.events: list[tuple[str, str, dict[str, object]]] = []
        self.fake = FakeQwenClient()

        async def publish(kind: str, device_id: str, data: dict[str, object]) -> None:
            self.events.append((kind, device_id, data))

        self.runtime = AiRuntime(
            self.settings,
            self.database,
            publish,
            client=self.fake,  # type: ignore[arg-type]
        )
        await self.runtime.start()

    async def asyncTearDown(self) -> None:
        await self.runtime.close()
        self.temp.cleanup()

    async def test_story_generation_validates_wav_and_queues_device_command(self) -> None:
        story, duplicate = await self.runtime.create_story(
            "dev",
            {
                "theme": "月亮",
                "child_age_months": 18,
                "duration_seconds": 45,
                "language": "zh-CN",
                "style": "calm",
                "autoplay": True,
                "request_id": "mobile-1",
            },
        )
        self.assertFalse(duplicate)
        await self.runtime._story_queue.join()
        completed = await self.runtime.get_story("dev", story["story_id"])
        self.assertIsNotNone(completed)
        self.assertEqual(completed["status"], "queued_for_device")
        self.assertGreater(completed["audio_size_bytes"], 44)
        self.assertEqual(completed["audio_duration_ms"], 100)
        command = self.database.query_one("SELECT * FROM commands WHERE id=?", (completed["command_id"],))
        self.assertIn("story.audio.play", command["command_json"])
        audio_files = list((self.settings.data_dir / "stories").glob("AI*.WAV"))
        self.assertEqual(len(audio_files), 1)

        repeated, repeated_duplicate = await self.runtime.create_story(
            "dev",
            {
                "theme": "ignored",
                "autoplay": True,
                "request_id": "mobile-1",
            },
        )
        self.assertTrue(repeated_duplicate)
        self.assertEqual(repeated["story_id"], story["story_id"])

    async def test_story_prompt_requires_causal_plot_instead_of_ambient_prose(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "app" / "ai_service.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("具体目标", source)
        self.assertIn("至少两次行动", source)
        self.assertIn("事件之间必须有因果关系", source)
        self.assertIn("环境描写不得超过正文的20%", source)
        self.assertIn("按开端、阻碍、行动、转折、解决、安静收尾", source)

    async def test_attention_telemetry_runs_in_background_and_publishes_advisory(self) -> None:
        self.runtime.offer_telemetry(
            "dev",
            {
                "sequence": 7,
                "inference": {"safe": {"level": 2, "severity": 2}},
                "sensors": {"thermal": {"valid": 5120, "mean_cd": 3020}},
            },
        )
        await self.runtime._assessment_queue.join()
        advisories = await self.runtime.list_advisories("dev", 10)
        self.assertEqual(len(advisories), 1)
        self.assertEqual(advisories[0]["level"], "attention")
        self.assertTrue(advisories[0]["requires_user_attention"])
        self.assertTrue(any(kind == "ai_advisory" for kind, _, _ in self.events))

    async def test_new_request_reuses_active_story_for_the_same_device(self) -> None:
        now = "2026-08-14T00:00:00+00:00"
        self.database.execute(
            "INSERT INTO ai_story_jobs(story_id, device_id, request_id, status, request_json, "
            "model, tts_model, created_at, updated_at) "
            "VALUES(?, ?, ?, 'generating', '{}', ?, ?, ?, ?)",
            ("active-story", "dev", "first-request", "qwen-plus", "qwen-tts", now, now),
        )

        story, duplicate = await self.runtime.create_story(
            "dev",
            {"theme": "第二个主题", "request_id": "second-request"},
        )

        self.assertTrue(duplicate)
        self.assertEqual(story["story_id"], "active-story")
        rows = self.database.query_all("SELECT story_id FROM ai_story_jobs WHERE device_id=?", ("dev",))
        self.assertEqual([row["story_id"] for row in rows], ["active-story"])

    async def test_model_cannot_downgrade_local_urgent_evidence(self) -> None:
        async def misleading_chat(*_: object, **__: object) -> dict[str, object]:
            return {
                "level": "normal",
                "title": "状态正常",
                "summary": "无需关注。",
                "evidence": [],
                "actions": [],
                "confidence": 0.9,
                "requires_user_attention": False,
            }

        self.fake.chat_json = misleading_chat  # type: ignore[method-assign]
        self.runtime.offer_telemetry(
            "dev",
            {
                "sequence": 9,
                "inference": {"safe": {"level": 3, "severity": 4}},
            },
        )
        await self.runtime._assessment_queue.join()
        advisories = await self.runtime.list_advisories("dev", 1)
        self.assertEqual(advisories[0]["level"], "urgent")
        self.assertTrue(advisories[0]["requires_user_attention"])
        self.assertEqual(advisories[0]["title"], "RA8 本地安全告警")
        self.assertNotIn("正常", advisories[0]["summary"])
        self.assertTrue(any("RA8 本地融合或安全层" in item for item in advisories[0]["evidence"]))

    async def test_deterministic_attention_replaces_contradictory_model_text(self) -> None:
        async def misleading_chat(*_: object, **__: object) -> dict[str, object]:
            return {
                "level": "normal",
                "title": "婴儿状态正常",
                "summary": "没有任何问题。",
                "evidence": ["毛毯置信度较高"],
                "actions": [],
                "confidence": 0.9,
                "requires_user_attention": False,
            }

        self.fake.chat_json = misleading_chat  # type: ignore[method-assign]
        self.runtime.offer_telemetry(
            "dev",
            {
                "sequence": 11,
                "inference": {"alarm": [1, 1, 7, 1, 1, 100], "severity": 1, "faults": 0},
                "vision": {"s": 6, "ss": [0, 0, 950, 0, 0]},
                "sensors": {
                    "thermal": {"frame_quality": 0, "valid": 5120},
                    "environment": {"co2": 4200},
                },
            },
        )
        await self.runtime._assessment_queue.join()
        advisory = (await self.runtime.list_advisories("dev", 1))[0]
        self.assertEqual(advisory["level"], "attention")
        self.assertEqual(advisory["title"], "RA8 本地安全状态需要关注")
        self.assertNotIn("毛毯", " ".join(advisory["evidence"]))
        self.assertTrue(any("CO2" in item for item in advisory["evidence"]))

    async def test_model_cannot_invent_urgent_without_local_urgent_evidence(self) -> None:
        async def alarmist_chat(*_: object, **__: object) -> dict[str, object]:
            return {
                "level": "urgent",
                "title": "立即查看",
                "summary": "模型认为需要紧急处理。",
                "evidence": ["没有本地紧急证据"],
                "actions": ["查看设备"],
                "confidence": 0.99,
                "requires_user_attention": True,
            }

        self.fake.chat_json = alarmist_chat  # type: ignore[method-assign]
        self.runtime.offer_telemetry(
            "dev",
            {
                "sequence": 10,
                "inference": {"safe": {"level": 0, "severity": 0}},
                "vision": {
                    "s": 6,
                    "q": 51,
                    "r": 7,
                    "fe": 1,
                    "ss": [120, 80, 990, 20, 0],
                    "d": [[2, 990, 10, 20, 100, 80]],
                },
                "sensors": {"thermal": {"frame_quality": 0, "valid": 5120}},
            },
        )
        await self.runtime._assessment_queue.join()
        advisories = await self.runtime.list_advisories("dev", 1)
        self.assertEqual(advisories[0]["level"], "normal")
        self.assertFalse(advisories[0]["requires_user_attention"])
        self.assertIn("不构成风险", advisories[0]["evidence"][0])

    async def test_qwen_tts_http_contract_requests_pcm16_wav_at_16khz(self) -> None:
        audio = pcm_wav()

        async def handler(request: httpx.Request) -> httpx.Response:
            if request.url.path.endswith("/SpeechSynthesizer"):
                body = json.loads(request.content)
                self.assertEqual(body["model"], "qwen-audio-3.0-tts-flash")
                self.assertEqual(body["input"]["voice"], "longanhuan_v3.6")
                self.assertEqual(body["input"]["format"], "wav")
                self.assertEqual(body["input"]["sample_rate"], 16000)
                return httpx.Response(
                    200,
                    json={"output": {"audio": {"url": "https://audio.test/story.wav"}}},
                )
            self.assertEqual(str(request.url), "https://audio.test/story.wav")
            return httpx.Response(200, content=audio)

        http_client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        qwen = QwenClient(self.settings, http_client)
        try:
            synthesized = await qwen.synthesize_wav("测试故事")
        finally:
            await http_client.aclose()
        self.assertEqual(synthesized, audio)

    async def test_gateway_ack_means_queued_for_playback_not_completed(self) -> None:
        story, _ = await self.runtime.create_story(
            "dev",
            {
                "theme": "星星",
                "autoplay": True,
                "request_id": "mobile-ack-state",
            },
        )
        await self.runtime._story_queue.join()
        generated = await self.runtime.get_story("dev", story["story_id"])
        self.assertIsNotNone(generated)
        await self.runtime.command_acked(
            "dev",
            int(generated["command_id"]),
            True,
            {"message": "stored and queued"},
        )
        acknowledged = await self.runtime.get_story("dev", story["story_id"])
        self.assertEqual(acknowledged["status"], "queued_for_playback")

    async def test_busy_story_transfer_keeps_verified_audio_ready(self) -> None:
        story, _ = await self.runtime.create_story(
            "dev",
            {
                "theme": "星星",
                "autoplay": True,
                "request_id": "mobile-busy-state",
            },
        )
        await self.runtime._story_queue.join()
        generated = await self.runtime.get_story("dev", story["story_id"])
        self.assertIsNotNone(generated)
        await self.runtime.command_acked(
            "dev",
            int(generated["command_id"]),
            False,
            {"message": "another story transfer is in progress"},
        )
        acknowledged = await self.runtime.get_story("dev", story["story_id"])
        self.assertEqual(acknowledged["status"], "ready")
        self.assertEqual(acknowledged["error"], "")

    async def test_story_filename_token_collision_is_retried(self) -> None:
        self.database.execute(
            "INSERT INTO ai_story_jobs(story_id, device_id, status, request_json, created_at, updated_at) "
            "VALUES(?, ?, 'failed', '{}', 'now', 'now')",
            ("abcdef11111111111111111111111111", "dev"),
        )
        candidates = [
            uuid.UUID("abcdef00-0000-0000-0000-000000000000"),
            uuid.UUID("12345600-0000-0000-0000-000000000000"),
        ]
        with patch("app.ai_service.uuid.uuid4", side_effect=candidates):
            story, duplicate = await self.runtime.create_story(
                "dev",
                {"theme": "星星", "autoplay": False, "request_id": "token-retry"},
            )
        self.assertFalse(duplicate)
        self.assertTrue(story["story_id"].startswith("123456"))
        await self.runtime._story_queue.join()
        audio_files = [path.name for path in (self.settings.data_dir / "stories").glob("AI*.WAV")]
        self.assertIn("AI123456.WAV", audio_files)

    async def test_long_story_appends_non_repeating_continuations(self) -> None:
        continuation_calls = 0

        async def short_story_chat(_: str, user_prompt: str, **__: object) -> dict[str, object]:
            nonlocal continuation_calls
            task = json.loads(user_prompt).get("task", "")
            if task.startswith("只续写"):
                continuation_calls += 1
                return {
                    "title": "长故事",
                    "story_text": ("新的星光慢慢照进小床，" * 22) + f"第{continuation_calls}段安静结束。",
                }
            return {"title": "长故事", "story_text": "月光轻轻落在小床边。" * 18}

        self.fake.chat_json = short_story_chat  # type: ignore[method-assign]
        story, _ = await self.runtime.create_story(
            "dev",
            {
                "theme": "月光",
                "duration_seconds": 180,
                "autoplay": False,
                "request_id": "long-continuation-test",
            },
        )
        await self.runtime._story_queue.join()
        completed = await self.runtime.get_story("dev", story["story_id"])
        self.assertEqual(completed["status"], "ready")
        self.assertGreaterEqual(continuation_calls, 1)
        self.assertGreater(len("".join(completed["story_text"].split())), 500)

    def test_invalid_tts_shape_is_rejected(self) -> None:
        with self.assertRaises(AiServiceError):
            validate_story_wav(pcm_wav(channels=2), 4 * 1024 * 1024)

    def test_streaming_placeholder_wav_is_normalized_to_physical_length(self) -> None:
        streaming = bytearray(pcm_wav(frames=1600))
        struct.pack_into("<I", streaming, 4, 0x7FFFFFBF)
        struct.pack_into("<I", streaming, 40, 0x7FFFFF9B)
        info, normalized = _normalize_story_wav(bytes(streaming), 4 * 1024 * 1024)
        self.assertEqual(info["duration_ms"], 100)
        self.assertEqual(struct.unpack_from("<I", normalized, 4)[0], len(normalized) - 8)
        self.assertEqual(struct.unpack_from("<I", normalized, 40)[0], len(normalized) - 44)

    def test_long_story_audio_downsamples_to_fit_transport_budget(self) -> None:
        audio, info = _merge_story_wavs(
            [pcm_wav(frames=15000), pcm_wav(frames=15000)],
            40000,
        )
        self.assertLessEqual(len(audio), 40000)
        self.assertEqual(info["sample_rate"], 8000)
        self.assertGreater(info["duration_ms"], 1800)

    def test_long_story_prefers_8khz_before_hitting_hard_limit(self) -> None:
        audio, info = _merge_story_wavs(
            [pcm_wav(frames=600000), pcm_wav(frames=600000)],
            4 * 1024 * 1024,
        )
        self.assertEqual(info["sample_rate"], 8000)
        self.assertLess(len(audio), 2 * 1024 * 1024)
        self.assertGreater(info["duration_ms"], 70000)

    def test_vision_arrays_are_semantic_and_blanket_is_not_a_fault(self) -> None:
        summary = _compact_telemetry({
            "sequence": 22,
            "vision": {
                "s": 6, "q": 51, "r": 7, "fe": 1,
                "ss": [120, 80, 990, 20, 0],
                "d": [[2, 990, 10, 20, 100, 80]],
            },
            "sensors": {"thermal": {"frame_quality": 0, "valid": 5120}},
            "inference": {"safe": {"level": 0, "severity": 0}},
        })
        self.assertEqual(summary["vision"]["frame_sequence"], 51)
        self.assertNotIn("frame_quality", summary["vision"])
        self.assertEqual(summary["vision"]["status_label"], "ready")
        self.assertEqual(summary["vision"]["class_scores"]["blanket"]["score_x1000"], 990)
        self.assertEqual(summary["vision"]["detections"][0]["class_name"], "blanket")
        self.assertEqual(summary["thermal"]["frame_quality_label"], "normal")
        self.assertEqual(_local_required_level(summary), "normal")

    def test_high_co2_is_deterministic_attention(self) -> None:
        summary = _compact_telemetry({
            "sensors": {
                "environment": {"co2": 4275},
                "thermal": {"frame_quality": 0, "valid": 5120},
            },
            "vision": {"s": 6, "r": 7, "fe": 1},
            "inference": {"safe": {"level": 0, "severity": 0}},
        })
        self.assertEqual(_local_required_level(summary), "attention")

    def test_configurable_environment_warning_requires_baby_in_bed(self) -> None:
        active = _compact_telemetry({
            "environment_policy": {
                "baby_in_bed": True,
                "active": True,
                "exceeded": [
                    {"name": "gas_resistance", "value": 8000, "threshold": 10000, "direction": "low"}
                ],
            },
            "sensors": {"thermal": {"frame_quality": 0, "valid": 5120}},
            "inference": {"safe": {"level": 0, "severity": 0}},
        })
        empty_bed = _compact_telemetry({
            "environment_policy": {
                "baby_in_bed": False,
                "active": False,
                "exceeded": [],
            },
            "sensors": {"thermal": {"frame_quality": 0, "valid": 5120}},
            "inference": {"safe": {"level": 0, "severity": 0}},
        })

        self.assertEqual(_local_required_level(active), "attention")
        self.assertEqual(_local_required_level(empty_bed), "normal")

    def test_current_flat_fusion_schema_retains_local_alarm(self) -> None:
        summary = _compact_telemetry({
            "inference": {
                "state": 8, "severity": 4, "sources": 17,
                "valid": 63, "faults": 0, "alarm": [3, 8, 42, 2, 1, 256],
                "comfort": [10, 8, 91, 1, 1],
            },
            "sensors": {
                "thermal": {"frame_quality": 0, "valid": 5120},
                "warm_light": {"s": 1, "on": 0},
                "soother": {"s": 1, "on": 1},
            },
            "vision": {"s": 6, "r": 7, "fe": 1},
        })
        self.assertEqual(summary["fusion"]["alarm_level"], 3)
        self.assertEqual(summary["fusion"]["severity"], 4)
        self.assertEqual(summary["fusion"]["comfort"]["recommended_light"], "on")
        self.assertFalse(summary["fusion"]["comfort"]["recommend_pat"])
        self.assertEqual(summary["actuators"]["warm_light"]["on"], 0)
        self.assertEqual(summary["actuators"]["soother"]["on"], 1)
        self.assertEqual(_local_required_level(summary), "urgent")
