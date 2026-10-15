from __future__ import annotations

import io
import struct
import sys
import time
import wave
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app


def pcm_wav() -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(struct.pack("<1600h", *([250] * 1600)))
    return output.getvalue()


class FakeQwenClient:
    async def close(self) -> None:
        return None

    async def chat_json(self, system_prompt: str, _: str, **__: object) -> dict[str, object]:
        if "睡前故事作者" in system_prompt:
            return {
                "title": "星星回家",
                "story_text": "小星星沿着安静的月光回到云朵家里，盖好柔软的小毯子，慢慢睡着了。",
                "age_range": "0-3岁",
                "estimated_seconds": 45,
                "safety_notes": [],
            }
        return {
            "level": "normal",
            "title": "状态平稳",
            "summary": "当前没有需要关注的变化。",
            "evidence": [],
            "actions": [],
            "confidence": 0.8,
            "requires_user_attention": False,
        }

    async def synthesize_wav(self, _: str) -> bytes:
        return pcm_wav()


class AiApiTest(TestCase):
    def setUp(self) -> None:
        self.temp = TemporaryDirectory()
        data = Path(self.temp.name)
        root = Path(__file__).resolve().parents[1]
        settings = Settings(
            root_dir=root,
            data_dir=data,
            database_path=data / "db.sqlite3",
            artifact_dir=data / "artifacts",
            device_keys={"dev": "secret"},
            ai_enabled=True,
            dashscope_api_key="test-only-key",
        )
        app = create_app(settings)
        app.state.ai_runtime.client = FakeQwenClient()
        self.context = TestClient(app)
        self.client = self.context.__enter__()
        self.device_headers = {"X-Device-ID": "dev", "X-Device-Key": "secret"}
        self.client.post(
            "/api/v1/device/hello",
            headers=self.device_headers,
            json={"firmware": "test", "hardware": "p4"},
        ).raise_for_status()

    def tearDown(self) -> None:
        self.context.__exit__(None, None, None)
        self.temp.cleanup()

    def _wait_story(self, story_id: str, status: str) -> dict[str, object]:
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            response = self.client.get(f"/api/mobile/v1/devices/dev/stories/{story_id}")
            response.raise_for_status()
            story = response.json()["story"]
            if story["status"] == status:
                return story
            time.sleep(0.02)
        self.fail(f"story did not reach {status}")

    def _wait_dashboard_ai(self, *, stories: int = 0, advisories: int = 0) -> dict[str, object]:
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            response = self.client.get("/api/dashboard/devices/dev/ai")
            response.raise_for_status()
            body = response.json()
            if len(body["stories"]) >= stories and len(body["advisories"]) >= advisories:
                return body
            time.sleep(0.02)
        self.fail("dashboard AI state did not update")

    def test_mobile_story_to_authenticated_device_audio_and_strong_ack(self) -> None:
        created = self.client.post(
            "/api/mobile/v1/devices/dev/stories",
            json={
                "theme": "星星",
                "duration_seconds": 45,
                "autoplay": True,
                "request_id": "mobile-http-1",
            },
        )
        created.raise_for_status()
        story_id = created.json()["story"]["story_id"]
        ready = self._wait_story(story_id, "queued_for_device")

        command = self.client.get(
            "/api/v1/commands/poll", headers=self.device_headers
        ).json()["command"]
        self.assertEqual(command["command"], "story.audio.play")
        self.assertEqual(command["args"]["story_id"], story_id)

        audio_path = command["args"]["path"]
        self.assertEqual(self.client.get(audio_path).status_code, 401)
        audio = self.client.get(audio_path, headers=self.device_headers)
        audio.raise_for_status()
        self.assertEqual(len(audio.content), ready["audio_size_bytes"])
        self.assertEqual(audio.headers["x-content-sha256"], command["args"]["sha256"])

        acknowledged = self.client.post(
            f"/api/v1/commands/{command['id']}/ack",
            headers=self.device_headers,
            json={"ok": True, "result": {"message": "stored and queued"}},
        )
        acknowledged.raise_for_status()
        self._wait_story(story_id, "queued_for_playback")

    def test_dashboard_can_generate_story_and_force_telemetry_assessment(self) -> None:
        story_response = self.client.post(
            "/api/dashboard/devices/dev/ai/stories",
            json={"theme": "月亮", "duration_seconds": 45, "autoplay": False},
        )
        story_response.raise_for_status()
        story_id = story_response.json()["story"]["story_id"]
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            state = self._wait_dashboard_ai(stories=1)
            story = next(item for item in state["stories"] if item["story_id"] == story_id)
            if story["status"] == "ready":
                break
            time.sleep(0.02)
        else:
            self.fail("dashboard story did not become ready")

        telemetry = self.client.post(
            "/api/v1/telemetry",
            headers=self.device_headers,
            json={
                "sequence": 7,
                "sensors": {"thermal": {"valid": 5120, "mean_cd": 3020}},
                "inference": {"safe": {"level": 0, "severity": 0}},
            },
        )
        telemetry.raise_for_status()
        state = self.client.get("/api/dashboard/devices/dev/ai")
        state.raise_for_status()
        self.assertEqual(state.json()["advisories"], [])
        requested = self.client.post("/api/dashboard/devices/dev/ai/assessment")
        requested.raise_for_status()
        self.assertTrue(requested.json()["accepted"])
        second = self._wait_dashboard_ai(advisories=1)
        self.assertTrue(second["status"]["enabled"])
