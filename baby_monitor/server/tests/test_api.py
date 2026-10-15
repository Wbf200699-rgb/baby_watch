import io
import asyncio
import base64
import struct
import zipfile
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from unittest import TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.config import Settings
from app.main import create_app
from app import protocol as protocol_module
from app.protocol import MediaBroker, decode_s3_audio_frame, decode_s3_video_frame


S3_AUDIO_HEADER = struct.Struct(">4sBBHIQIHHHH")


def s3_audio_frame(sequence: int, *, timestamp_us: int = 1_000_000, sample: int = 0) -> bytes:
    payload = struct.pack("<320h", *([sample] * 320))
    return S3_AUDIO_HEADER.pack(
        b"S3MD", 1, 2, 0, sequence, timestamp_us, len(payload), 16000, 320, 1, 16
    ) + payload


def s3_video_frame(
    sequence: int,
    *,
    timestamp_us: int = 2_000_000,
    width: int = 640,
    height: int = 480,
    quality: int = 14,
    payload: bytes = b"\xff\xd8jpeg-test\xff\xd9",
) -> bytes:
    return S3_AUDIO_HEADER.pack(
        b"S3MD", 1, 1, 0, sequence, timestamp_us, len(payload), width, height, quality, 0
    ) + payload


class ApiTest(TestCase):
    def setUp(self) -> None:
        self.temp = TemporaryDirectory()
        data = Path(self.temp.name)
        root = Path(__file__).resolve().parents[1]
        settings = Settings(
            root,
            data,
            data / "db.sqlite3",
            data / "artifacts",
            {"dev": "secret", "other": "other-secret"},
        )
        self.client_context = TestClient(create_app(settings))
        self.client = self.client_context.__enter__()
        self.headers = {"X-Device-ID": "dev", "X-Device-Key": "secret"}

    def tearDown(self) -> None:
        self.client_context.__exit__(None, None, None)
        self.temp.cleanup()

    def test_dashboard_assets_are_fresh_and_show_fat_83_paths(self) -> None:
        index = self.client.get("/")
        index.raise_for_status()
        self.assertEqual(index.headers["cache-control"], "no-store")
        self.assertIn("/CAMERA/C*.JPG", index.text)
        self.assertNotIn("/CAMERA/CAM*.JPG", index.text)
        self.assertIn("telemetry-history.js?v=20260730-p4x-realtime-4", index.text)
        self.assertIn("thermal-display.js?v=20260731-thermal-quality-1", index.text)
        self.assertIn("matrix-display.js?v=20260801-pressure-ai-1", index.text)
        self.assertIn("styles.css?v=20260813-soother-switch-1", index.text)
        self.assertIn("app.js?v=20260821-resource-recording-1", index.text)
        self.assertIn("vision-live-state", index.text)
        self.assertIn("vision-box-preview", index.text)
        self.assertIn('data-tab="ai"', index.text)
        self.assertIn('id="speaker-volume"', index.text)
        self.assertIn('id="warm-light-toggle"', index.text)
        self.assertIn('id="soother-toggle"', index.text)
        self.assertIn('id="resource-recording-start"', index.text)
        self.assertIn('id="resource-recording-stop"', index.text)
        self.assertIn('id="resource-recording-download"', index.text)
        self.assertIn('id="comfort-recommendation"', index.text)
        self.assertIn('id="environment-threshold-form"', index.text)
        self.assertIn('role="switch"', index.text)
        self.assertIn('maxlength="8"', index.text)

        script = self.client.get("/static/app.js")
        script.raise_for_status()
        self.assertEqual(script.headers["cache-control"], "no-store")
        self.assertIn("采集受阻", script.text)
        self.assertIn("FAT I/O 错误", script.text)
        self.assertIn("telemetry.inference.v", script.text)
        self.assertNotIn('return `${kind === "audio" ? "mic" : "camera"}_${timestamp}`', script.text)
        live_start = script.text.index("function appendLiveTelemetry")
        live_end = script.text.index("function applyLiveMessage", live_start)
        self.assertIn("renderRecordingTests(state.snapshot)", script.text[live_start:live_end])
        self.assertIn("function microphoneActiveSlotPeak(microphone)", script.text)
        self.assertIn("microphone.wslot == null", script.text)
        self.assertIn(
            "Number(microphone.wslot) === 1 ? microphone.wopk : microphone.wepk",
            script.text,
        )
        self.assertIn("Number(slotPeak ?? microphone.peak)", script.text)
        self.assertIn(
            'microphoneActiveSlotPeak(get(row, "sensors.microphone"))',
            script.text,
        )
        self.assertIn("connectDashboardRealtime()", script.text)
        self.assertIn("refreshDashboardLiveState()", script.text)
        self.assertIn("function renderEventRows()", script.text)
        event_branch = script.text.index('message.kind === "event"')
        advisory_branch = script.text.index('message.kind === "ai_story"', event_branch)
        self.assertNotIn("scheduleRefresh()", script.text[event_branch:advisory_branch])
        self.assertIn('socket.close(4000, "realtime timeout")', script.text)
        self.assertIn("buildPressureBaseline", script.text)
        self.assertIn("pressurePeakHold", script.text)
        self.assertIn('action: "light.set"', script.text)
        self.assertIn('action: "soother.set"', script.text)
        self.assertIn("telemetry.sensors?.warm_light", script.text)
        self.assertIn("telemetry.sensors?.soother", script.text)
        self.assertIn("warmLightConfirmationTimeout", script.text)
        self.assertIn("sootherPatConfirmationTimeout", script.text)
        self.assertIn("ra8CommandLinkState", script.text)
        self.assertIn("P4 GPIO7 TX → RA8 P602 RX 未握手", script.text)
        self.assertIn("renderComfortRecommendation", script.text)
        self.assertIn("thermalCandidateMask", script.text)
        self.assertIn("/ai/assessment", script.text)
        self.assertIn("`volume${volume}`", script.text)

        history_helper = self.client.get("/static/telemetry-history.js")
        history_helper.raise_for_status()
        self.assertEqual(history_helper.headers["cache-control"], "no-store")
        self.assertIn("mergeTelemetryHistory", history_helper.text)

        matrix_helper = self.client.get("/static/matrix-display.js")
        matrix_helper.raise_for_status()
        self.assertEqual(matrix_helper.headers["cache-control"], "no-store")
        self.assertIn("thermalCandidateMask", matrix_helper.text)
        self.assertIn("pressurePeakHold", matrix_helper.text)

    def test_dashboard_realtime_wss_publishes_latest_events(self) -> None:
        with self.client.websocket_connect("/api/dashboard/realtime/ws") as websocket:
            self.client.post(
                "/api/v1/device/hello",
                headers=self.headers,
                json={"firmware": "realtime-test"},
            ).raise_for_status()
            message = websocket.receive_json()
            self.assertEqual(message["kind"], "device")
            self.assertEqual(message["device_id"], "dev")
            self.assertTrue(message["data"]["online"])

    def test_sd_collection_channels_are_independent(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        created = self.client.post("/api/dashboard/sessions", json={"device_id": "dev", "name": "trial"}).json()
        session_id = created["session_id"]
        self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=audio").raise_for_status()
        command = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(command["command"], "collection.start")
        self.assertEqual(command["args"]["kinds"], ["audio"])
        self.client.post(f"/api/v1/commands/{command['id']}/ack", headers=self.headers, json={"ok": True}).raise_for_status()
        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        self.assertEqual(snapshot["sessions"][0]["metadata"]["active_kinds"], ["audio"])

    def test_camera_collection_queues_independent_sd_command(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        created = self.client.post(
            "/api/dashboard/sessions",
            json={"device_id": "dev", "name": "covered face", "metadata": {"data_kind": "camera"}},
        ).json()
        session_id = created["session_id"]

        started = self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=camera").json()
        self.assertEqual(started["storage"], "sd")
        command = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(command["command"], "collection.start")
        self.assertEqual(command["args"]["kinds"], ["camera"])

        self.client.post(
            f"/api/v1/commands/{command['id']}/ack", headers=self.headers, json={"ok": True}
        ).raise_for_status()
        self.client.post(f"/api/dashboard/sessions/{session_id}/stop?kind=camera").raise_for_status()
        stop_command = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(stop_command["command"], "collection.stop")
        self.assertEqual(stop_command["args"]["kinds"], ["camera"])

    def test_media_recording_controls_validate_names_and_queue_device_commands(self) -> None:
        audio = self.client.post(
            "/api/dashboard/recordings/audio/start",
            json={"device_id": "dev", "filename": "cry-test.WAV"},
        )
        audio.raise_for_status()
        self.assertEqual(audio.json()["expected_path"], "/MIC/cry-test.WAV")
        command = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(command["command"], "collection.start")
        self.assertEqual(command["args"], {
            "kinds": ["audio"],
            "storage": "sd",
            "purpose": "media_test",
            "filename": "cry-test",
        })
        self.client.post(
            f"/api/v1/commands/{command['id']}/ack",
            headers=self.headers,
            json={"ok": True},
        ).raise_for_status()

        camera = self.client.post(
            "/api/dashboard/recordings/camera/capture",
            json={"device_id": "dev", "filename": "face_01.jpeg"},
        )
        camera.raise_for_status()
        self.assertEqual(camera.json()["expected_path"], "/CAMERA/face_01.JPG")
        command = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(command["command"], "collection.capture")
        self.assertEqual(command["args"]["filename"], "face_01")

        invalid = self.client.post(
            "/api/dashboard/recordings/audio/start",
            json={"device_id": "dev", "filename": "../escape.wav"},
        )
        self.assertEqual(invalid.status_code, 422)
        too_long = self.client.post(
            "/api/dashboard/recordings/camera/capture",
            json={"device_id": "dev", "filename": "camera001.jpg"},
        )
        self.assertEqual(too_long.status_code, 422)
        unsupported = self.client.post(
            "/api/dashboard/recordings/audio/capture",
            json={"device_id": "dev", "filename": "test"},
        )
        self.assertEqual(unsupported.status_code, 404)

    def test_resource_recording_captures_unique_metrics_and_exports_csv(self) -> None:
        started = self.client.post(
            "/api/dashboard/resource-recordings/start", json={"device_id": "dev"}
        )
        started.raise_for_status()
        session_id = started.json()["session"]["session_id"]
        self.assertFalse(started.json()["already_active"])

        resource = {
            "sequence": 11,
            "age_ms": 25,
            "window_ms": 1000,
            "flags": 7,
            "cpu0_usage_permille": 123,
            "cpu1_usage_permille": 456,
            "npu_usage_permille": 789,
            "cpu0_heap_free": 100000,
            "cpu0_heap_min": 90000,
            "cpu1_heap_free": 80000,
            "cpu1_heap_min": 70000,
            "npu_busy_ms": 789,
            "inference_count": 42,
            "vision_total_ms": 321,
            "cpu1_tick": 9988,
        }
        for telemetry_sequence, resource_sequence in ((101, 11), (102, 11), (103, 12)):
            payload = {
                "sequence": telemetry_sequence,
                "device_time_ms": 5000 + telemetry_sequence,
                "ra8_tick": 9000 + telemetry_sequence,
                "health": {"ra8_resources": {**resource, "sequence": resource_sequence}},
            }
            self.client.post("/api/v1/telemetry", headers=self.headers, json=payload).raise_for_status()

        status = self.client.get("/api/dashboard/resource-recordings/status?device_id=dev").json()
        self.assertTrue(status["active"])
        self.assertEqual(status["session"]["sample_count"], 2)
        self.assertEqual(
            self.client.app.state.db.query_one("SELECT COUNT(*) AS count FROM commands")["count"], 0
        )

        stopped = self.client.post(
            "/api/dashboard/resource-recordings/stop", json={"device_id": "dev"}
        )
        stopped.raise_for_status()
        self.assertFalse(stopped.json()["already_stopped"])
        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 104,
                "health": {"ra8_resources": {**resource, "sequence": 13}},
            },
        ).raise_for_status()
        samples = self.client.get(
            f"/api/dashboard/sessions/{session_id}/samples?limit=20"
        ).json()
        self.assertEqual(len(samples), 2)
        self.assertEqual(samples[0]["features"]["npu_usage_permille"], 789)

        exported = self.client.get(
            f"/api/dashboard/resource-recordings/{session_id}/csv"
        )
        exported.raise_for_status()
        self.assertIn("cpu0_usage_percent,cpu1_usage_percent,npu_usage_percent", exported.text)
        self.assertIn("12.3,45.6,78.9", exported.text)

    def test_temperature_collection_is_labeled_and_does_not_capture_other_channels(self) -> None:
        created = self.client.post(
            "/api/dashboard/sessions",
            json={
                "device_id": "dev",
                "name": "sleeping temperature",
                "target_labels": ["sleeping"],
                "metadata": {"capture_label": "sleeping", "data_kind": "temperature"},
            },
        ).json()
        session_id = created["session_id"]
        started = self.client.post(
            f"/api/dashboard/sessions/{session_id}/start?kind=temperature"
        ).json()
        self.assertIsNone(started["command_id"])
        self.assertEqual(started["storage"], "server")

        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 7,
                "ra8_tick": 123,
                "sensors": {
                    "bed_temperature": {"seq": 4, "t_mc": [23500, 23600]},
                    "microphone": {"seq": 8, "peak": 900},
                },
            },
        ).raise_for_status()
        self.client.post(
            "/api/v1/matrices/pressure?sequence=3&rows=2&cols=2",
            headers=self.headers,
            content=b"\x01\x00" * 4,
        ).raise_for_status()

        samples = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0]["kind"], "temperature")
        self.assertEqual(samples[0]["label"], "sleeping")
        self.assertEqual(samples[0]["features"]["t_mc"], [23500, 23600])
        session = next(
            item for item in self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sessions"]
            if item["session_id"] == session_id
        )
        self.assertEqual(session["metadata"]["storage"], "server")

    def test_telemetry(self) -> None:
        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 1,
                "health": {"sd_ready": True},
                "vision": {
                    "q": 9,
                    "f": 4,
                    "s": 6,
                    "r": 7,
                    "w": 320,
                    "h": 240,
                    "ss": [812, 744, 115, 0, 0],
                    "oc": [0, 851, 851, 102, 47],
                    "bh": [1, 734, 162, 734, 31, 73],
                    "d": [[0, 812, 12, 18, 220, 180]],
                },
                "inference": {
                    "sv": 2,
                    "ev": 9,
                    "ctx": [1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0],
                    "alarm": [0, 0, 0, 0, 0, 0],
                    "vv": [9, 100, 1, 1, 0, 250],
                    "ai": [],
                },
            },
        )
        self.assertEqual(response.status_code, 200)
        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        telemetry = snapshot["telemetry"]
        self.assertTrue(telemetry["health"]["sd_ready"])
        self.assertEqual(telemetry["vision"]["r"], 7)
        self.assertEqual(telemetry["vision"]["d"][0][1], 812)
        self.assertEqual(snapshot["fusion_view"]["schema_version"], 2)
        self.assertEqual(snapshot["vision_overlay"]["boxes"][0]["class_id"], 0)

    def test_bme688_alias_is_normalized_for_rest_history_mobile_and_websocket(self) -> None:
        first_bme688 = {
            "s": 3,
            "addr": 0x76,
            "t_mc": 24_125,
            "rh_mp": 48_500,
            "p_pa": 100_825,
            "gas_ohm": 125_400,
            "flags": 0x07,
            "last": 118_000,
            "err": 0,
        }
        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 40,
                "ra8_tick": 120_000,
                "sensors": {"environment": {"co2": 720}, "bme688": first_bme688},
                "health": {"ra8_online": True},
            },
        )
        response.raise_for_status()

        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        self.assertNotIn("bme688", snapshot["telemetry"]["sensors"])
        self.assertEqual(
            snapshot["telemetry"]["sensors"]["environment"]["bme688"],
            first_bme688,
        )
        history = self.client.get("/api/dashboard/history?device_id=dev&limit=10").json()
        self.assertEqual(
            history[-1]["sensors"]["environment"]["bme688"]["gas_ohm"],
            125_400,
        )
        mobile = self.client.get("/api/mobile/v1/devices/dev/snapshot").json()
        self.assertEqual(
            mobile["telemetry"]["sensors"]["environment"]["bme688"]["p_pa"],
            100_825,
        )

        with self.client.websocket_connect("/api/v1/device/stream", headers=self.headers) as websocket:
            websocket.send_json({
                "kind": "telemetry",
                "payload": {
                    "sequence": 41,
                    "ra8_tick": 121_000,
                    "sensors": {
                        "bme688": {
                            **first_bme688,
                            "p_pa": 100_900,
                            "gas_ohm": 130_000,
                            "last": 121_000,
                        }
                    },
                    "health": {"ra8_online": True},
                },
            })
            self.assertEqual(websocket.receive_json()["kind"], "telemetry_ack")
            live = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["telemetry"]
            self.assertNotIn("bme688", live["sensors"])
            self.assertEqual(live["sensors"]["environment"]["bme688"]["p_pa"], 100_900)

    def test_environment_thresholds_are_persistent_and_presence_gated(self) -> None:
        self.client.post(
            "/api/v1/device/hello", headers=self.headers, json={"firmware": "policy-test"}
        ).raise_for_status()
        updated = self.client.put(
            "/api/dashboard/devices/dev/environment-thresholds",
            json={
                "co2_max_ppm": 900,
                "temperature_max_milli_c": 28_000,
                "humidity_max_milli_percent": 60_000,
                "humidity_min_milli_percent": 30_000,
                "gas_min_ohm": 20_000,
                "body_temperature_max_deci_c": 375,
            },
        )
        updated.raise_for_status()
        self.assertEqual(updated.json()["thresholds"]["co2_max_ppm"], 900)
        threshold_command = self.client.get(
            "/api/v1/commands/poll", headers=self.headers
        ).json()["command"]
        self.assertEqual(threshold_command["command"], "ra8.command")
        self.assertEqual(
            threshold_command["args"]["payload"],
            "environment_thresholds_v2:900,28000,60000,20000,375,30000",
        )
        self.client.post(
            f"/api/v1/commands/{threshold_command['id']}/ack",
            headers=self.headers,
            json={"ok": True},
        ).raise_for_status()

        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 42,
                "sensors": {
                    "environment": {
                        "co2": 1000,
                        "t_mc": 29_000,
                        "rh_mp": 65_000,
                        "bme688": {"s": 3, "flags": 6, "gas_ohm": 15_000},
                    }
                },
                "inference": {
                    "fusion": {
                        "ctx": [1, 0, 0, 0, 0, 0, 0, 2],
                        "ex": [0, 0, 0, 0, 700, 0, 0, 0, 12, 0, 0, 380],
                    }
                },
            },
        ).raise_for_status()

        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        evaluation = snapshot["environment_policy"]["evaluation"]
        self.assertTrue(evaluation["active"])
        self.assertEqual(len(evaluation["exceeded"]), 5)
        self.assertTrue(evaluation["observed"]["body_temperature_valid"])
        self.assertEqual(evaluation["observed"]["body_temperature_deci_c"], 380)
        self.assertTrue(snapshot["telemetry"]["environment_policy"]["active"])

        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 43,
                "sensors": {
                    "environment": {
                        "co2": 1000,
                        "t_mc": 29_000,
                        "rh_mp": 65_000,
                        "bme688": {"s": 3, "flags": 6, "gas_ohm": 15_000},
                    }
                },
                "inference": {
                    "fusion": {
                        "ctx": [2, 0, 0, 0, 0, 0, 0, 2],
                        "ex": [0, 0, 0, 0, 700, 0, 0, 0, 12, 0, 0, 390],
                    }
                },
            },
        ).raise_for_status()
        absent_evaluation = self.client.get(
            "/api/dashboard/snapshot?device_id=dev"
        ).json()["environment_policy"]["evaluation"]
        self.assertFalse(absent_evaluation["active"])
        self.assertEqual(absent_evaluation["exceeded"], [])

        persisted = self.client.get(
            "/api/dashboard/devices/dev/environment-thresholds"
        ).json()
        self.assertEqual(persisted["thresholds"]["gas_min_ohm"], 20_000)
        self.assertEqual(persisted["thresholds"]["body_temperature_max_deci_c"], 375)
        self.assertEqual(persisted["thresholds"]["humidity_min_milli_percent"], 30_000)

    def test_environment_policy_prefers_ra8_reported_thresholds(self) -> None:
        self.client.post(
            "/api/v1/device/hello", headers=self.headers, json={"firmware": "limits-test"}
        ).raise_for_status()
        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 1,
                "sensors": {
                    "environment": {
                        "co2": 1000,
                        "t_mc": 25000,
                        "rh_mp": 40000,
                        "bme688": {"s": 3, "flags": 6, "gas_ohm": 100000},
                    }
                },
                "inference": {
                    "fusion": {
                        "ctx": [1, 0, 0, 0, 0, 0, 0, 1],
                        "ex": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 360],
                        "limits": [900, 30000, 70000, 10000, 375, 7],
                    }
                },
            },
        ).raise_for_status()

        policy = self.client.get(
            "/api/dashboard/devices/dev/environment-thresholds"
        ).json()
        self.assertEqual(policy["thresholds"]["co2_max_ppm"], 900)
        self.assertEqual(policy["thresholds"]["source"], "hardware")
        self.assertFalse(policy["hardware_synced"])
        self.assertEqual(policy["evaluation"]["threshold_source"], "hardware")
        self.assertEqual(
            policy["evaluation"]["thresholds"]["humidity_min_milli_percent"],
            30_000,
        )
        self.assertTrue(policy["evaluation"]["active"])

    def test_overlay_calibration_is_persistent_and_mobile_visible(self) -> None:
        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 2,
                "vision": {"q": 2, "f": 2, "w": 320, "h": 240, "fe": 1, "d": [[0, 900, 80, 60, 160, 120]]},
                "inference": {"vv": [2, 50, 1, 1, 0, 200]},
            },
        ).raise_for_status()
        response = self.client.put(
            "/api/dashboard/devices/dev/overlay-calibration",
            json={
                "enabled": True,
                "mode": "homography",
                "source_points": [[0, 0], [1, 0], [1, 1], [0, 1]],
                "target_points": [[0.1, 0.2], [0.9, 0.2], [0.9, 0.8], [0.1, 0.8]],
                "thermal_enabled": True,
                "thermal_source_points": [[0.426, 0.206], [0.541, 0.541]],
                "thermal_target_points": [[0.78, 0.27], [0.47, 0.43]],
                "thermal_opacity": 0.32,
            },
        )
        response.raise_for_status()
        self.assertEqual(response.json()["calibration"]["status"], "calibrated")
        mobile = self.client.get("/api/mobile/v1/devices/dev/snapshot").json()
        self.assertEqual(mobile["overlay_calibration"]["mode"], "homography")
        self.assertTrue(mobile["overlay_calibration"]["thermal"]["enabled"])
        self.assertAlmostEqual(mobile["overlay_calibration"]["thermal"]["opacity"], 0.32)
        self.assertAlmostEqual(mobile["vision_overlay"]["boxes"][0]["x"], 0.3, places=6)
        self.assertIn("fusion", mobile)

    def test_sensor_diagnostics_identify_component_and_temperature_channel_faults(self) -> None:
        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 12,
                "ra8_tick": 3400,
                "sensors": {
                    "thermal": {
                        "s": 3,
                        "rx": 10248,
                        "valid": 8,
                        "crc_rx": 0x1234,
                        "crc_calc": 0x1234,
                    },
                    "bed_temperature": {
                        "s": 3,
                        "ok": 0x1B,
                        "err": 0x04,
                        "t_mc": [24125, 24250, 0, 24400, 24500],
                        "addr": [0x48, 0x48, 0, 0x48, 0x48],
                    },
                    "pressure": {"s": 3, "ok": 0, "err": 1, "fsp": 8},
                    "microphone": {
                        "s": 2,
                        "rate": 16000,
                        "bits": 32,
                        "read": 42,
                        "cb": 43,
                        "err": 0,
                        "fsp": 0,
                        "sdr": 1,
                        "sds": 11,
                    },
                    "camera": {"s": 4, "probe": 1, "probe_ok": 1, "fsp": 0, "ver": "VC0703 1.00"},
                },
                "inference": {
                    "valid": 0x09,
                    "faults": 0x03,
                },
                "health": {
                    "ra8_online": True,
                    "ra8_status_age_ms": 120,
                    "ra8_link": {"rx": 100, "err": 0, "ovf": 0},
                },
            },
        )
        response.raise_for_status()

        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        items = {item["id"]: item for item in diagnostics["items"]}

        self.assertEqual(diagnostics["overall"], "fault")
        self.assertEqual(items["thermal"]["status"], "fault")
        self.assertIn("8/5120", items["thermal"]["detail"])
        self.assertEqual(items["pressure"]["status"], "warning")
        self.assertIn("本地正常", items["pressure"]["summary"])
        self.assertEqual(items["microphone"]["status"], "ok")
        self.assertEqual(items["camera"]["status"], "ok")
        temperature_channels = {channel["id"]: channel for channel in items["bed_temperature"]["channels"]}
        self.assertEqual(temperature_channels["ch0"]["status"], "ok")
        self.assertEqual(temperature_channels["ch2"]["status"], "fault")
        self.assertIn("扫描错误", temperature_channels["ch2"]["detail"])

    def test_link_diagnostics_identify_uart_direction_and_payload_overflow(self) -> None:
        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 15,
                "health": {
                    "ra8_online": False,
                    "ra8_status_age_ms": 0xFFFFFFFF,
                    "uart_valid_frames": 0,
                    "uart_status_frames": 0,
                    "ra8_link": {},
                    "gateway": {
                        "uart_rx_bytes": 0,
                        "ready_tx_count": 12,
                        "ready_tx_errors": 0,
                    },
                },
            },
        )
        response.raise_for_status()

        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        link = next(item for item in diagnostics["items"] if item["id"] == "ra8_link")
        self.assertEqual(link["summary"], "网关未收到 RA8 UART 字节")
        self.assertEqual(link["metrics"]["ready_tx_count"], 12)
        self.assertEqual(link["metrics"]["uart_rx_bytes"], 0)

        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 16,
                "health": {
                    "ra8_online": False,
                    "ra8_status_age_ms": 0xFFFFFFFF,
                    "uart_valid_frames": 0,
                    "uart_status_frames": 0,
                    "ra8_link": {},
                    "gateway": {
                        "uart_rx_bytes": 40960,
                        "ready_tx_count": 14,
                        "ready_tx_errors": 0,
                        "uart_active_baud": 460800,
                        "uart_baud_locked": False,
                        "uart_baud_switches": 3,
                        "uart_magic_aa": 7,
                        "uart_sync_headers": 0,
                        "uart_sample_hex": "FF00FF00",
                    },
                },
            },
        )
        response.raise_for_status()
        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        link = next(item for item in diagnostics["items"] if item["id"] == "ra8_link")
        self.assertEqual(link["summary"], "收到 UART 字节但没有有效协议帧")
        self.assertEqual(link["metrics"]["uart_active_baud"], 460800)
        self.assertFalse(link["metrics"]["uart_baud_locked"])
        self.assertEqual(link["metrics"]["uart_sample_hex"], "FF00FF00")

        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 17,
                "health": {
                    "ra8_online": True,
                    "ra8_status_age_ms": 80,
                    "uart_valid_frames": 20,
                    "uart_status_frames": 20,
                    "gateway": {"uart_rx_bytes": 8192, "ready_tx_count": 4, "ready_tx_errors": 0},
                    "ra8_link": {
                        "ready": 1,
                        "ready_n": 4,
                        "st_try": 22,
                        "st_ok": 20,
                        "st_len": 4102,
                        "st_detail": 3,
                        "st_long": 2,
                        "st_uart": 0,
                    },
                },
            },
        )
        response.raise_for_status()
        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        link = next(item for item in diagnostics["items"] if item["id"] == "ra8_link")
        self.assertEqual(link["status"], "fault")
        self.assertEqual(link["summary"], "RA8 当前状态包已退化")
        self.assertEqual(link["metrics"]["status_payload_too_long"], 2)

        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 18,
                "health": {
                    "ra8_online": True,
                    "ra8_status_age_ms": 90,
                    "uart_rx_bytes": 16384,
                    "uart_valid_frames": 24,
                    "uart_status_frames": 24,
                    "ready_tx_count": 8,
                    "ready_tx_errors": 0,
                    "gateway": {
                        "uart_rx_bytes": 0,
                        "ready_tx_count": 0,
                        "ready_tx_errors": 0,
                        "uart_active_baud": 921600,
                        "uart_baud_locked": True,
                    },
                    "ra8_link": {
                        "ready": 1,
                        "ready_n": 8,
                        "st_try": 26,
                        "st_ok": 24,
                        "st_len": 3850,
                        "st_detail": 2,
                        "st_long": 7,
                        "st_uart": 3,
                    },
                },
            },
        )
        response.raise_for_status()
        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        link = next(item for item in diagnostics["items"] if item["id"] == "ra8_link")
        self.assertEqual(link["status"], "ok")
        self.assertEqual(link["summary"], "双向在线")
        self.assertEqual(link["metrics"]["uart_rx_bytes"], 16384)
        self.assertEqual(link["metrics"]["ready_tx_count"], 8)
        self.assertEqual(link["metrics"]["status_detail"], 2)

    def test_link_diagnostics_marks_unconfirmed_reverse_command_path_as_fault(self) -> None:
        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 19,
                "health": {
                    "ra8_online": True,
                    "ra8_status_age_ms": 70,
                    "uart_rx_bytes": 32768,
                    "uart_valid_frames": 42,
                    "uart_status_frames": 42,
                    "ready_tx_count": 16,
                    "ready_tx_errors": 0,
                    "gateway": {"uart_active_baud": 921600, "uart_baud_locked": True},
                    "ra8_link": {
                        "ready": 0,
                        "ready_n": 0,
                        "st_try": 44,
                        "st_ok": 42,
                        "st_len": 3900,
                        "st_detail": 2,
                    },
                },
            },
        )
        response.raise_for_status()

        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        link = next(item for item in diagnostics["items"] if item["id"] == "ra8_link")
        channels = {channel["id"]: channel for channel in link["channels"]}
        self.assertEqual(link["status"], "fault")
        self.assertEqual(link["summary"], "反向命令链路故障")
        self.assertTrue(link["metrics"]["reverse_link_failed"])
        self.assertFalse(link["metrics"]["reverse_command_ready"])
        self.assertEqual(channels["s3_to_ra8"]["status"], "fault")
        self.assertIn("控制命令不可用", link["detail"])

    def test_sensor_diagnostics_report_camera_sd_writer_error(self) -> None:
        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 13,
                "sensors": {
                    "camera": {
                        "s": 5,
                        "probe": 1,
                        "probe_ok": 1,
                        "fsp": 0,
                        "ver": "VC0703 1.00",
                        "wst": 4,
                        "werr": 5,
                        "saved": 6,
                        "file": "C0000021.JPG",
                    }
                },
                "health": {"ra8_online": True},
            },
        )
        response.raise_for_status()

        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        camera = next(item for item in diagnostics["items"] if item["id"] == "camera")
        channels = {channel["id"]: channel for channel in camera["channels"]}

        self.assertEqual(camera["status"], "fault")
        self.assertEqual(camera["summary"], "SD 图像保存故障")
        self.assertIn("errno 5", camera["detail"])
        self.assertEqual(channels["sci3"]["status"], "ok")
        self.assertEqual(channels["sd_writer"]["status"], "fault")

    def test_sensor_diagnostics_reject_false_sd_ready_and_camera_writer_timeout(self) -> None:
        response = self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 14,
                "ra8_tick": 20000,
                "sensors": {
                    "microphone": {"sdr": 1, "sds": 5, "sdtry": 37, "sdrec": 36},
                    "camera": {
                        "s": 4,
                        "probe": 1,
                        "probe_ok": 1,
                        "fsp": 20,
                        "to": 50,
                        "fail_stage": 4,
                        "wst": 0,
                        "werr": 0,
                        "saved": 40,
                        "file": "C0000051.JPG",
                    },
                },
                "health": {"ra8_online": True},
            },
        )
        response.raise_for_status()

        diagnostics = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["sensor_diagnostics"]
        items = {item["id"]: item for item in diagnostics["items"]}

        self.assertEqual(items["sd_card"]["status"], "fault")
        self.assertEqual(items["sd_card"]["summary"], "挂载流程未完整完成")
        self.assertTrue(items["sd_card"]["metrics"]["incomplete_ready"])
        self.assertEqual(items["camera"]["status"], "fault")
        self.assertEqual(items["camera"]["summary"], "JPEG 写入线程无响应")
        self.assertTrue(items["camera"]["metrics"]["writer_ack_timeout"])

    def test_mobile_snapshot_devices_and_matrix_contract(self) -> None:
        self.client.post(
            "/api/v1/device/hello",
            headers=self.headers,
            json={"firmware": "gateway-1", "capabilities": ["ra8", "s3_video"]},
        ).raise_for_status()
        self.client.post(
            "/api/v1/telemetry",
            headers=self.headers,
            json={
                "sequence": 21,
                "ra8_tick": 1234,
                "wifi_rssi": -47,
                "sensors": {
                    "bed_temperature": {
                        "s": 2,
                        "ok": 31,
                        "err": 0,
                        "t_mc": [23125, 23250, 23375, 23500, 23625],
                        "addr": [72, 72, 72, 72, 72],
                    },
                    "audio_player": {
                        "s": 1,
                        "track": 1,
                        "count": 2,
                        "vol": 50,
                        "file": "track001.wav",
                    },
                    "warm_light": {"s": 1, "on": 0, "pin": 711, "ah": 1},
                    "soother": {
                        "s": 1,
                        "on": 1,
                        "pin": 914,
                        "cmd_seq": 1,
                    },
                },
                "inference": {
                    "sv": 7,
                    "comfort": [6, 2, 88, 1, 0],
                },
                "health": {"ra8_online": True},
            },
        ).raise_for_status()
        self.client.post(
            "/api/v1/matrices/pressure?sequence=8&rows=2&cols=2",
            headers=self.headers,
            content=struct.pack("<4h", 1, 5, 9, 13),
        ).raise_for_status()

        capabilities = self.client.get("/api/mobile/v1/capabilities").json()
        self.assertEqual(capabilities["protocol_version"], 1)
        self.assertIn("audio.play_track", {item["action"] for item in capabilities["commands"]})
        self.assertIn("light.set", {item["action"] for item in capabilities["commands"]})
        self.assertIn("soother.set", {item["action"] for item in capabilities["commands"]})
        commands = {item["action"]: item for item in capabilities["commands"]}
        self.assertTrue(commands["light.set"]["idempotent"])
        self.assertTrue(commands["soother.set"]["idempotent"])
        self.assertEqual(
            capabilities["environment_sensors"]["bme688"]["path"],
            "telemetry.sensors.environment.bme688",
        )
        self.assertEqual(
            capabilities["environment_sensors"]["bme688"]["gas_semantics"],
            "raw_resistance_only",
        )
        self.assertFalse(capabilities["environment_sensors"]["bme688"]["bsec"])
        self.assertEqual(
            capabilities["actuators"]["warm_light"]["state_path"],
            "telemetry.sensors.warm_light",
        )
        self.assertEqual(
            capabilities["actuators"]["warm_light"]["command_action"],
            "light.set",
        )
        self.assertEqual(capabilities["actuators"]["warm_light"]["boot_default"], "off")
        self.assertEqual(capabilities["actuators"]["warm_light"]["control"], "switch")
        self.assertEqual(
            capabilities["actuators"]["warm_light"]["recommendation_path"],
            "fusion.comfort.recommended_light",
        )
        self.assertEqual(capabilities["actuators"]["warm_light"]["confirmation_timeout_ms"], 8000)
        self.assertEqual(capabilities["actuators"]["soother_pat"]["pin"], "P914")
        self.assertEqual(capabilities["actuators"]["soother_pat"]["boot_default"], "off")
        self.assertEqual(capabilities["actuators"]["soother_pat"]["control"], "switch")
        self.assertEqual(
            capabilities["actuators"]["soother_pat"]["recommendation_path"],
            "fusion.comfort.recommend_pat",
        )
        self.assertFalse(capabilities["actuators"]["soother_pat"]["physical_feedback"])
        self.assertTrue(capabilities["actuators"]["comfort_recommendation"]["never_auto_execute"])
        self.assertTrue(
            capabilities["ai"]["telemetry_advisory"]["automatic_requires_mobile_stream"]
        )

        devices = self.client.get("/api/mobile/v1/devices").json()
        self.assertEqual(devices["type"], "mobile.devices")
        self.assertEqual(devices["devices"][0]["id"], "dev")
        self.assertEqual(devices["devices"][0]["firmware"], "gateway-1")

        snapshot = self.client.get("/api/mobile/v1/devices/dev/snapshot").json()
        self.assertEqual(snapshot["type"], "mobile.snapshot")
        self.assertEqual(snapshot["telemetry"]["sequence"], 21)
        self.assertEqual(snapshot["telemetry"]["sensors"]["audio_player"]["vol"], 50)
        self.assertEqual(snapshot["telemetry"]["sensors"]["warm_light"]["on"], 0)
        self.assertEqual(snapshot["telemetry"]["sensors"]["soother"]["on"], 1)
        self.assertEqual(snapshot["fusion"]["comfort"]["recommended_light"], "on")
        self.assertTrue(snapshot["fusion"]["comfort"]["recommend_pat"])
        self.assertTrue(snapshot["fusion"]["comfort"]["requires_confirmation"])
        self.assertEqual(snapshot["matrices"]["pressure"]["sequence"], 8)
        self.assertNotIn("values", snapshot["matrices"]["pressure"])
        self.assertEqual(snapshot["links"]["video_ws"], "/api/mobile/v1/devices/dev/media/video/ws")

        matrix = self.client.get("/api/mobile/v1/devices/dev/matrices/pressure").json()
        self.assertEqual(matrix["type"], "mobile.matrix")
        self.assertEqual(matrix["matrix"]["values"], [1, 5, 9, 13])

    def test_mobile_command_mapping_status_and_idempotency(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        request = {
            "action": "audio.play_track",
            "args": {"track": 2},
            "request_id": "phone-request-001",
        }
        created = self.client.post("/api/mobile/v1/devices/dev/commands", json=request)
        created.raise_for_status()
        first = created.json()
        self.assertFalse(first["duplicate"])
        self.assertEqual(first["command"]["action"], "audio.play_track")
        command_id = first["command"]["id"]

        duplicate = self.client.post("/api/mobile/v1/devices/dev/commands", json=request).json()
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(duplicate["command"]["id"], command_id)

        polled = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(polled["id"], command_id)
        self.assertEqual(polled["command"], "ra8.command")
        self.assertEqual(polled["args"]["payload"], "play track2")
        self.client.post(
            f"/api/v1/commands/{command_id}/ack",
            headers=self.headers,
            json={"ok": True, "result": {"message": "forwarded to RA8"}},
        ).raise_for_status()

        status = self.client.get(f"/api/mobile/v1/devices/dev/commands/{command_id}").json()["command"]
        self.assertEqual(status["status"], "acked")
        self.assertEqual(status["result"]["message"], "forwarded to RA8")
        self.assertEqual(status["request_id"], "phone-request-001")

        volume = self.client.post(
            "/api/mobile/v1/devices/dev/commands",
            json={"action": "audio.set_volume", "args": {"percent": 35}},
        )
        volume.raise_for_status()
        polled_volume = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(polled_volume["args"]["payload"], "volume35")

        light_on = self.client.post(
            "/api/mobile/v1/devices/dev/commands",
            json={"action": "light.set", "args": {"on": True}},
        )
        light_on.raise_for_status()
        polled_light_on = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(polled_light_on["args"]["payload"], "light_on")

        light_off = self.client.post(
            "/api/mobile/v1/devices/dev/commands",
            json={"action": "light.set", "args": {"on": False}},
        )
        light_off.raise_for_status()
        polled_light_off = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(polled_light_off["args"]["payload"], "light_off")

        invalid_light = self.client.post(
            "/api/mobile/v1/devices/dev/commands",
            json={"action": "light.set", "args": {"on": 1}},
        )
        self.assertEqual(invalid_light.status_code, 422)

        soother = self.client.post(
            "/api/mobile/v1/devices/dev/commands",
            json={"action": "soother.set", "args": {"on": True}},
        )
        soother.raise_for_status()
        polled_soother = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(polled_soother["command"], "soother.set")
        self.assertEqual(polled_soother["args"], {"on": True})

        invalid_soother = self.client.post(
            "/api/mobile/v1/devices/dev/commands",
            json={"action": "soother.set", "args": {"on": 1}},
        )
        self.assertEqual(invalid_soother.status_code, 422)

        invalid = self.client.post(
            "/api/mobile/v1/devices/dev/commands",
            json={"action": "audio.set_volume", "args": {"percent": 101}},
        )
        self.assertEqual(invalid.status_code, 422)

    def test_automatic_ai_assessment_requires_mobile_stream_presence(self) -> None:
        runtime = self.client.app.state.ai_runtime
        presence = self.client.app.state.mobile_presence
        telemetry_calls: list[tuple[str, dict]] = []
        event_calls: list[tuple[str, dict]] = []
        original_telemetry = runtime.offer_telemetry
        original_event = runtime.offer_event
        runtime.offer_telemetry = lambda device_id, body: telemetry_calls.append((device_id, body))
        runtime.offer_event = lambda device_id, body: event_calls.append((device_id, body))
        try:
            self.client.post(
                "/api/v1/telemetry", headers=self.headers, json={"sequence": 1}
            ).raise_for_status()
            self.client.post(
                "/api/v1/events",
                headers=self.headers,
                json={
                    "event_id": 1,
                    "event_type": "crying",
                    "state": 1,
                    "severity": 2,
                    "confidence": 80,
                },
            ).raise_for_status()
            self.assertEqual(telemetry_calls, [])
            self.assertEqual(event_calls, [])

            presence.connect("dev")
            self.assertTrue(presence.connected("dev"))
            self.assertEqual(presence.count("dev"), 1)
            self.client.post(
                "/api/v1/telemetry", headers=self.headers, json={"sequence": 2}
            ).raise_for_status()
            self.client.post(
                "/api/v1/events",
                headers=self.headers,
                json={
                    "event_id": 2,
                    "event_type": "crying",
                    "state": 1,
                    "severity": 2,
                    "confidence": 80,
                },
            ).raise_for_status()
            self.assertEqual(len(telemetry_calls), 1)
            self.assertEqual(len(event_calls), 1)

            presence.disconnect("dev")
            self.assertFalse(presence.connected("dev"))
            self.client.post(
                "/api/v1/telemetry", headers=self.headers, json={"sequence": 3}
            ).raise_for_status()
            self.assertEqual(len(telemetry_calls), 1)
        finally:
            runtime.offer_telemetry = original_telemetry
            runtime.offer_event = original_event

    def test_latest_batch_ingests_telemetry_and_matrices(self) -> None:
        telemetry = b'{"sequence":9,"wifi_rssi":-48,"health":{"ra8_online":true}}'
        pressure = struct.pack("<4h", 1, 2, 3, 4)
        thermal = struct.pack("<4h", 215, 220, 225, 230)
        header = struct.pack(
            "<4sBBHIIHHHH", b"RA8B", 1, 3, len(telemetry), 17, 23, 2, 2, 2, 2
        )
        response = self.client.post(
            "/api/v1/batch", headers=self.headers, content=header + telemetry + pressure + thermal
        )
        response.raise_for_status()
        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        self.assertEqual(snapshot["telemetry"]["sequence"], 9)
        self.assertEqual(snapshot["matrices"]["pressure"]["sequence"], 17)
        self.assertEqual(snapshot["matrices"]["pressure"]["values"], [1, 2, 3, 4])
        self.assertEqual(snapshot["matrices"]["thermal"]["sequence"], 23)
        self.assertEqual(snapshot["matrices"]["thermal"]["values"], [215, 220, 225, 230])

        live = self.client.get("/api/dashboard/devices/dev/live-state").json()
        self.assertEqual(live["device_id"], "dev")
        self.assertTrue(live["device"]["online"])
        self.assertIn("telemetry_status", live)
        self.assertIn("matrix_status", live)
        self.assertIn("sensor_diagnostics", live)
        self.assertNotIn("matrices", live)
        self.assertNotIn("events", live)

    def test_latest_batch_rejects_truncated_matrix(self) -> None:
        telemetry = b'{"sequence":1}'
        header = struct.pack(
            "<4sBBHIIHHHH", b"RA8B", 1, 1, len(telemetry), 2, 0, 2, 2, 0, 0
        )
        response = self.client.post(
            "/api/v1/batch", headers=self.headers, content=header + telemetry + b"\x00\x00"
        )
        self.assertEqual(response.status_code, 400)

    def test_latest_batch_delivers_pending_command(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        created = self.client.post(
            "/api/dashboard/commands",
            json={"device_id": "dev", "command": "media.preview.start", "args": {"video": True}},
        ).json()
        telemetry = b'{"sequence":2}'
        header = struct.pack("<4sBBHIIHHHH", b"RA8B", 1, 0, len(telemetry), 0, 0, 0, 0, 0, 0)
        response = self.client.post("/api/v1/batch", headers=self.headers, content=header + telemetry)
        response.raise_for_status()
        self.assertEqual(response.json()["command"]["id"], created["id"])
        self.assertEqual(response.json()["command"]["command"], "media.preview.start")

    def test_latest_batch_v2_carries_live_video_and_audio(self) -> None:
        telemetry = b'{"sequence":31}'
        jpeg = b"\xff\xd8preview\xff\xd9"
        audio = struct.pack("<4h", -100, 0, 100, 200)
        header = struct.pack(
            "<4sBBHIIHHHHIIIIIBBH",
            b"RA8B", 2, 12, len(telemetry), 0, 0, 0, 0, 0, 0,
            7, len(jpeg), 9, len(audio), 8000, 1, 16, 0,
        )
        response = self.client.post(
            "/api/v1/batch", headers=self.headers, content=header + telemetry + jpeg + audio
        )
        response.raise_for_status()
        self.assertEqual(response.json()["video"], {"sequence": 7, "bytes": len(jpeg)})
        self.assertEqual(response.json()["audio"], {"sequence": 9, "bytes": len(audio)})
        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        self.assertEqual(snapshot["media"]["video"]["sequence"], 7)
        self.assertEqual(snapshot["media"]["audio"]["sequence"], 9)
        self.assertEqual(snapshot["media"]["audio"]["sample_rate"], 8000)

    def test_websocket_device_stream_multiplexes_telemetry_and_matrix(self) -> None:
        with self.client.websocket_connect("/api/v1/device/stream", headers=self.headers) as websocket:
            websocket.send_json({
                "kind": "hello",
                "payload": {"firmware": "wss-test", "metadata": {"local_ip": "192.0.2.2"}},
            })
            self.assertEqual(websocket.receive_json()["kind"], "hello_ack")
            websocket.send_json({
                "kind": "telemetry",
                "payload": {"sequence": 12, "wifi_rssi": -51, "health": {"ra8_online": True}},
            })
            telemetry_ack = websocket.receive_json()
            self.assertEqual(telemetry_ack["kind"], "telemetry_ack")
            self.assertEqual(telemetry_ack["payload"]["sequence"], 12)

            matrix = struct.pack(">4sBBHHI", b"RA8M", 1, 2, 2, 2, 27) + struct.pack("<4h", -2, 0, 3, 8)
            websocket.send_bytes(matrix)
            matrix_ack = websocket.receive_json()
            self.assertEqual(matrix_ack["kind"], "matrix_ack")
            self.assertEqual(matrix_ack["payload"]["sequence"], 27)

            created = self.client.post(
                "/api/dashboard/commands",
                json={"device_id": "dev", "command": "status.refresh", "args": {}},
            ).json()
            command = websocket.receive_json()
            self.assertEqual(command["kind"], "command")
            self.assertEqual(command["payload"]["id"], created["id"])
            websocket.send_json({
                "kind": "command_ack",
                "payload": {"id": created["id"], "ok": True, "result": {"forwarded": True}},
            })
            self.assertEqual(websocket.receive_json()["kind"], "command_ack_received")

        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        self.assertEqual(snapshot["telemetry"]["sequence"], 12)
        self.assertEqual(snapshot["matrices"]["pressure"]["values"], [-2, 0, 3, 8])
        self.assertEqual(snapshot["commands"][0]["status"], "acked")

    def test_device_reports_local_ip_and_server_tracks_forwarded_ip(self) -> None:
        response = self.client.post(
            "/api/v1/device/hello",
            headers={**self.headers, "X-Forwarded-For": "203.0.113.7"},
            json={"firmware": "gateway", "metadata": {"local_ip": "192.168.4.2", "mac": "AA:BB:CC:DD:EE:FF"}},
        )
        response.raise_for_status()
        device = self.client.get("/api/dashboard/devices").json()[0]
        self.assertEqual(device["ip_address"], "203.0.113.7")
        self.assertEqual(device["metadata"]["local_ip"], "192.168.4.2")

    def test_live_media_is_bounded_in_memory_and_exposed_in_snapshot(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        video = self.client.post(
            "/api/v1/media/video?sequence=9",
            headers={**self.headers, "Content-Type": "image/jpeg"},
            content=b"\xff\xd8test\xff\xd9",
        )
        video.raise_for_status()
        audio = self.client.post(
            "/api/v1/media/audio?sequence=4&sample_rate=16000&channels=1&bits=16",
            headers={**self.headers, "Content-Type": "application/octet-stream"},
            content=b"\x00\x00\x01\x00",
        )
        audio.raise_for_status()
        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        self.assertEqual(snapshot["media"]["video"]["sequence"], 9)
        self.assertEqual(snapshot["media"]["audio"]["sequence"], 4)
        self.assertEqual(snapshot["media"]["audio"]["sample_rate"], 16000)

    def test_s3_audio_wss_relays_frames_and_controls_capture(self) -> None:
        frame = s3_audio_frame(17, timestamp_us=9_876_543, sample=123)
        with self.client.websocket_connect(
            "/api/v1/media/device/audio", headers=self.headers
        ) as device:
            initial = device.receive_json()
            self.assertEqual(initial["type"], "media.control")
            self.assertFalse(initial["enabled"])
            self.assertEqual(initial["subscribers"], 0)

            with self.client.websocket_connect(
                "/api/dashboard/media/dev/audio/ws"
            ) as dashboard:
                start = device.receive_json()
                self.assertTrue(start["enabled"])
                self.assertEqual(start["subscribers"], 1)
                device.send_bytes(frame)
                self.assertEqual(dashboard.receive_bytes(), frame)

                snapshot = self.client.get(
                    "/api/dashboard/snapshot?device_id=dev"
                ).json()
                self.assertEqual(snapshot["media"]["audio"]["sequence"], 17)
                self.assertEqual(snapshot["media"]["audio"]["timestamp_us"], 9_876_543)
                self.assertEqual(snapshot["media"]["audio"]["transport"], "s3md_wss")
                self.assertEqual(snapshot["media"]["audio_uploaders"], 1)
                self.assertEqual(snapshot["media"]["audio_wss_listeners"], 1)

            stop = device.receive_json()
            self.assertFalse(stop["enabled"])
            self.assertEqual(stop["subscribers"], 0)
            self.assertGreater(stop["sequence"], start["sequence"])

    def test_s3_audio_wss_reports_invalid_frames_in_diagnostics(self) -> None:
        malformed = s3_audio_frame(1)[:-2]
        with self.client.websocket_connect(
            "/api/v1/media/device/audio", headers=self.headers
        ) as device:
            device.receive_json()
            device.send_bytes(malformed)
            error = device.receive_json()
            self.assertEqual(error["type"], "media.error")

        diagnostics = self.client.get(
            "/api/dashboard/snapshot?device_id=dev"
        ).json()["media"]["audio_diagnostics"]
        self.assertEqual(diagnostics["invalid_frames"], 1)
        self.assertIn("payload length", diagnostics["last_invalid_frame_error"])

    def test_s3_video_wss_relays_latest_jpeg_and_controls_capture(self) -> None:
        frame = s3_video_frame(23, timestamp_us=12_345_678)
        with self.client.websocket_connect(
            "/api/v1/media/device/video", headers=self.headers
        ) as device:
            initial = device.receive_json()
            self.assertEqual(initial["type"], "media.control")
            self.assertFalse(initial["enabled"])
            self.assertEqual(initial["subscribers"], 0)
            self.assertEqual(initial["target_fps"], 8)

            with self.client.websocket_connect(
                "/api/dashboard/media/dev/video/ws"
            ) as dashboard:
                start = device.receive_json()
                self.assertTrue(start["enabled"])
                self.assertEqual(start["subscribers"], 1)
                device.send_bytes(frame)
                self.assertEqual(dashboard.receive_bytes(), frame)

                snapshot = self.client.get(
                    "/api/dashboard/snapshot?device_id=dev"
                ).json()
                video = snapshot["media"]["video"]
                self.assertEqual(video["sequence"], 23)
                self.assertEqual(video["timestamp_us"], 12_345_678)
                self.assertEqual(video["width"], 640)
                self.assertEqual(video["height"], 480)
                self.assertEqual(video["quality"], 14)
                self.assertEqual(video["transport"], "s3md_wss")
                self.assertEqual(snapshot["media"]["video_uploaders"], 1)
                self.assertEqual(snapshot["media"]["video_wss_listeners"], 1)
                diagnostics = snapshot["media"]["video_diagnostics"]
                self.assertEqual(diagnostics["video_device_connections"], 1)
                self.assertEqual(diagnostics["video_listeners"], 1)
                self.assertEqual(diagnostics["subscribers"], 1)
                self.assertEqual(diagnostics["received_frames"], 1)
                self.assertEqual(diagnostics["received_fps"], 0.0)
                self.assertEqual(diagnostics["queue_limit"], 1)
                self.assertEqual(diagnostics["subscriber_drops"], 0)
                self.assertEqual(diagnostics["invalid_frames"], 0)
                self.assertIsNone(diagnostics["last_invalid_frame_error"])

            stop = device.receive_json()
            self.assertFalse(stop["enabled"])
            self.assertEqual(stop["subscribers"], 0)
            self.assertGreater(stop["sequence"], start["sequence"])

    def test_media_control_state_is_refreshed_while_connection_is_idle(self) -> None:
        async def scenario() -> None:
            broker = MediaBroker()
            video = await broker.register_video_uploader("dev")
            audio = await broker.register_audio_uploader("dev")
            first_video = await broker.next_video_control("dev", video)
            first_audio = await broker.next_audio_control("dev", audio)
            self.assertFalse(first_video["enabled"])
            self.assertFalse(first_audio["enabled"])

            original = protocol_module.S3_MEDIA_CONTROL_REFRESH_SECONDS
            protocol_module.S3_MEDIA_CONTROL_REFRESH_SECONDS = 0.001
            try:
                refreshed_video = await broker.next_video_control("dev", video)
                refreshed_audio = await broker.next_audio_control("dev", audio)
            finally:
                protocol_module.S3_MEDIA_CONTROL_REFRESH_SECONDS = original

            self.assertGreater(refreshed_video["sequence"], first_video["sequence"])
            self.assertGreater(refreshed_audio["sequence"], first_audio["sequence"])
            self.assertFalse(refreshed_video["enabled"])
            self.assertFalse(refreshed_audio["enabled"])

        asyncio.run(scenario())

    def test_video_target_fps_is_configurable_between_eight_and_twelve(self) -> None:
        async def scenario() -> None:
            broker = MediaBroker(video_target_fps=10)
            queue = await broker.register_video_uploader("dev")
            control = await broker.next_video_control("dev", queue)
            self.assertEqual(control["target_fps"], 10)
            self.assertEqual(broker.status("dev")["video_target_fps"], 10)
            broker.unregister_video_uploader("dev", queue, 1000)
            replacement = await broker.register_video_uploader("dev")
            diagnostics = broker.status("dev")["video_diagnostics"]
            self.assertEqual(diagnostics["reconnects"], 1)
            self.assertEqual(diagnostics["last_close_code"], 1000)
            broker.unregister_video_uploader("dev", replacement, 1000)

        asyncio.run(scenario())
        with self.assertRaises(ValueError):
            MediaBroker(video_target_fps=13)

    def test_media_diagnostics_measure_timed_receive_rates(self) -> None:
        async def scenario() -> None:
            broker = MediaBroker(video_target_fps=10)
            for sequence in range(3):
                await broker.publish_s3_video(
                    "dev", decode_s3_video_frame(s3_video_frame(sequence))
                )
                await broker.publish_s3_audio(
                    "dev", decode_s3_audio_frame(s3_audio_frame(sequence))
                )
                await asyncio.sleep(0.01)
            status = broker.status("dev")
            self.assertGreater(status["video_diagnostics"]["received_fps"], 0.0)
            self.assertGreater(status["audio_diagnostics"]["received_fps"], 0.0)
            self.assertIsNotNone(status["video_diagnostics"]["frame_age_ms"])
            self.assertIsNotNone(status["audio_diagnostics"]["frame_age_ms"])

        asyncio.run(scenario())

    def test_mobile_media_websockets_relay_original_s3md_frames(self) -> None:
        video_frame = s3_video_frame(31)
        with self.client.websocket_connect(
            "/api/v1/media/device/video", headers=self.headers
        ) as device_video:
            device_video.receive_json()
            with self.client.websocket_connect(
                "/api/mobile/v1/devices/dev/media/video/ws"
            ) as mobile_video:
                control = device_video.receive_json()
                self.assertTrue(control["enabled"])
                device_video.send_bytes(video_frame)
                self.assertEqual(mobile_video.receive_bytes(), video_frame)

        audio_frame = s3_audio_frame(32, sample=456)
        with self.client.websocket_connect(
            "/api/v1/media/device/audio", headers=self.headers
        ) as device_audio:
            device_audio.receive_json()
            with self.client.websocket_connect(
                "/api/mobile/v1/devices/dev/media/audio/ws"
            ) as mobile_audio:
                control = device_audio.receive_json()
                self.assertTrue(control["enabled"])
                device_audio.send_bytes(audio_frame)
                self.assertEqual(mobile_audio.receive_bytes(), audio_frame)

    def test_s3_video_wss_rejects_credentials_and_malformed_frames(self) -> None:
        with self.assertRaises(WebSocketDisconnect) as rejected:
            with self.client.websocket_connect(
                "/api/v1/media/device/video",
                headers={"X-Device-ID": "dev", "X-Device-Key": "wrong"},
            ):
                pass
        self.assertEqual(rejected.exception.code, 1008)

        malformed = s3_video_frame(1, payload=b"not-a-jpeg")
        with self.client.websocket_connect(
            "/api/v1/media/device/video", headers=self.headers
        ) as device:
            device.receive_json()
            device.send_bytes(malformed)
            error = device.receive_json()
            self.assertEqual(error["type"], "media.error")
            with self.assertRaises(WebSocketDisconnect) as closed:
                device.receive_bytes()
            self.assertEqual(closed.exception.code, 1003)

        diagnostics = self.client.get(
            "/api/dashboard/snapshot?device_id=dev"
        ).json()["media"]["video_diagnostics"]
        self.assertEqual(diagnostics["invalid_frames"], 1)
        self.assertEqual(
            diagnostics["last_invalid_frame_error"],
            "JPEG payload is missing SOI or EOI marker",
        )
        self.assertIsNotNone(diagnostics["last_invalid_frame_at"])

    def test_s3_audio_wss_rejects_credentials_and_malformed_frames(self) -> None:
        with self.assertRaises(WebSocketDisconnect) as rejected:
            with self.client.websocket_connect(
                "/api/v1/media/device/audio",
                headers={"X-Device-ID": "dev", "X-Device-Key": "wrong"},
            ):
                pass
        self.assertEqual(rejected.exception.code, 1008)

        malformed = bytearray(s3_audio_frame(1))
        malformed[20:24] = (638).to_bytes(4, "big")
        with self.client.websocket_connect(
            "/api/v1/media/device/audio", headers=self.headers
        ) as device:
            device.receive_json()
            device.send_bytes(bytes(malformed))
            error = device.receive_json()
            self.assertEqual(error["type"], "media.error")
            with self.assertRaises(WebSocketDisconnect) as closed:
                device.receive_bytes()
            self.assertEqual(closed.exception.code, 1003)

    def test_s3_audio_subscriber_queue_drops_oldest_frames(self) -> None:
        async def scenario() -> None:
            broker = MediaBroker()
            subscription = broker.subscribe_s3_audio("dev")
            first_receive = asyncio.create_task(anext(subscription))
            await asyncio.sleep(0)
            frames = [s3_audio_frame(sequence) for sequence in range(10)]
            for frame in frames:
                await broker.publish_s3_audio("dev", decode_s3_audio_frame(frame))

            retained = [await first_receive]
            retained.extend([await anext(subscription) for _ in range(7)])
            self.assertEqual(retained, frames[2:])
            diagnostics = broker.status("dev")["audio_diagnostics"]
            self.assertEqual(diagnostics["received_frames"], len(frames))
            self.assertGreaterEqual(diagnostics["subscriber_drops"], 1)
            self.assertLessEqual(diagnostics["queue_depth"], diagnostics["queue_limit"])
            await subscription.aclose()

        asyncio.run(scenario())

    def test_s3_video_subscriber_queue_retains_only_latest_frame(self) -> None:
        async def scenario() -> None:
            broker = MediaBroker()
            subscription = broker.subscribe_s3_video("dev")
            first_receive = asyncio.create_task(anext(subscription))
            await asyncio.sleep(0)
            frames = [s3_video_frame(sequence) for sequence in range(3)]
            await broker.publish_s3_video("dev", decode_s3_video_frame(frames[0]))
            self.assertEqual(await first_receive, frames[0])
            await broker.publish_s3_video("dev", decode_s3_video_frame(frames[1]))
            await broker.publish_s3_video("dev", decode_s3_video_frame(frames[2]))
            self.assertEqual(await anext(subscription), frames[2])
            diagnostics = broker.status("dev")["video_diagnostics"]
            self.assertEqual(diagnostics["received_frames"], len(frames))
            self.assertGreaterEqual(diagnostics["subscriber_drops"], 1)
            self.assertLessEqual(diagnostics["queue_depth"], diagnostics["queue_limit"])
            await subscription.aclose()

        asyncio.run(scenario())

    def test_dashboard_audio_wss_uses_dashboard_basic_auth(self) -> None:
        data = Path(self.temp.name) / "media-auth"
        root = Path(__file__).resolve().parents[1]
        settings = Settings(
            root,
            data,
            data / "db.sqlite3",
            data / "artifacts",
            {"dev": "secret"},
            admin_user="admin",
            admin_password="password",
        )
        with TestClient(create_app(settings)) as client:
            with self.assertRaises(WebSocketDisconnect) as rejected:
                with client.websocket_connect(
                    "/api/dashboard/media/dev/audio/ws"
                ):
                    pass
            self.assertEqual(rejected.exception.code, 1008)

            authorization = base64.b64encode(b"admin:password").decode("ascii")
            with client.websocket_connect(
                "/api/dashboard/media/dev/audio/ws",
                headers={"Authorization": f"Basic {authorization}"},
            ):
                pass
            with self.assertRaises(WebSocketDisconnect) as rejected_video:
                with client.websocket_connect(
                    "/api/dashboard/media/dev/video/ws"
                ):
                    pass
            self.assertEqual(rejected_video.exception.code, 1008)
            with client.websocket_connect(
                "/api/dashboard/media/dev/video/ws",
                headers={"Authorization": f"Basic {authorization}"},
            ):
                pass
            with self.assertRaises(WebSocketDisconnect) as rejected_realtime:
                with client.websocket_connect("/api/dashboard/realtime/ws"):
                    pass
            self.assertEqual(rejected_realtime.exception.code, 1008)
            with client.websocket_connect(
                "/api/dashboard/realtime/ws",
                headers={"Authorization": f"Basic {authorization}"},
            ) as realtime:
                realtime.send_json({"type": "dashboard.ping", "sequence": 7})
                pong = realtime.receive_json()
                self.assertEqual(pong["kind"], "heartbeat_ack")
                self.assertEqual(pong["sequence"], 7)

            self.assertEqual(client.get("/api/mobile/v1/capabilities").status_code, 401)
            self.assertEqual(
                client.get(
                    "/api/mobile/v1/capabilities",
                    headers={"Authorization": f"Basic {authorization}"},
                ).status_code,
                200,
            )
            with self.assertRaises(WebSocketDisconnect) as rejected_mobile:
                with client.websocket_connect(
                    "/api/mobile/v1/devices/dev/media/video/ws"
                ):
                    pass
            self.assertEqual(rejected_mobile.exception.code, 1008)
            with client.websocket_connect(
                "/api/mobile/v1/devices/dev/media/video/ws",
                headers={"Authorization": f"Basic {authorization}"},
            ):
                pass

    def test_history_returns_oldest_to_newest(self) -> None:
        for sequence in (7, 8, 9):
            self.client.post(
                "/api/v1/telemetry",
                headers=self.headers,
                json={"sequence": sequence, "wifi_rssi": -60 + sequence},
            ).raise_for_status()
        history = self.client.get("/api/dashboard/history?device_id=dev&limit=10").json()
        self.assertEqual([row["sequence"] for row in history], [7, 8, 9])

    def test_runtime_history_pruning_keeps_latest_rows(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        for sequence in range(5):
            self.client.post(
                "/api/v1/telemetry",
                headers=self.headers,
                json={"sequence": sequence},
            ).raise_for_status()

        database = self.client.app.state.db
        database.prune_runtime_data("dev", telemetry_rows=2, log_rows=2, matrix_rows=2)
        rows = database.query_all("SELECT sequence FROM telemetry WHERE device_id=? ORDER BY id", ("dev",))
        self.assertEqual([row["sequence"] for row in rows], [3, 4])

    def test_event_retry_is_reported_as_duplicate_before_dashboard_ack(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        event = {
            "event_id": 17,
            "event_type": "near_edge",
            "state": 1,
            "severity": 4,
            "confidence": 92,
        }
        first = self.client.post("/api/v1/events", headers=self.headers, json=event).json()
        second = self.client.post("/api/v1/events", headers=self.headers, json=event).json()
        self.assertFalse(first["duplicate"])
        self.assertTrue(second["duplicate"])
        self.assertEqual(first["id"], second["id"])

    def test_reused_event_id_after_ra8_reboot_creates_a_new_mobile_event(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        first_event = {
            "event_id": 1,
            "event_type": "ra8_safety",
            "state": 4,
            "severity": 3,
            "confidence": 90,
            "occurred_at_ms": 1000,
        }
        first = self.client.post("/api/v1/events", headers=self.headers, json=first_event).json()
        self.client.app.state.db.execute(
            "UPDATE events SET received_at='2000-01-01T00:00:00.000+00:00' WHERE id=?",
            (first["id"],),
        )
        rebooted_event = {**first_event, "occurred_at_ms": 250}
        second = self.client.post("/api/v1/events", headers=self.headers, json=rebooted_event).json()

        self.assertFalse(second["duplicate"])
        self.assertNotEqual(first["id"], second["id"])
        events = self.client.get("/api/mobile/v1/devices/dev/events?limit=10").json()["events"]
        self.assertEqual(events[0]["id"], second["id"])

    def test_unacknowledged_command_is_redelivered(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        created = self.client.post(
            "/api/dashboard/commands",
            json={"device_id": "dev", "command": "status.refresh", "args": {}},
        ).json()

        first = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(first["id"], created["id"])
        self.assertEqual(first["delivery_attempt"], 1)
        self.assertIsNone(self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"])

        self.client.app.state.db.execute(
            "UPDATE commands SET sent_at='2000-01-01T00:00:00.000+00:00' WHERE id=?",
            (created["id"],),
        )
        retry = self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"]
        self.assertEqual(retry["id"], created["id"])
        self.assertEqual(retry["delivery_attempt"], 2)
        self.client.post(
            f"/api/v1/commands/{created['id']}/ack",
            headers=self.headers,
            json={"ok": True},
        ).raise_for_status()
        self.assertIsNone(self.client.get("/api/v1/commands/poll", headers=self.headers).json()["command"])

    def test_collection_samples_are_isolated_by_device_and_kind(self) -> None:
        other_headers = {"X-Device-ID": "other", "X-Device-Key": "other-secret"}
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        self.client.post("/api/v1/device/hello", headers=other_headers, json={"firmware": "test"}).raise_for_status()
        created = self.client.post(
            "/api/dashboard/sessions",
            json={"device_id": "dev", "name": "isolated"},
        ).json()
        session_id = created["session_id"]
        self.client.post(f"/api/dashboard/sessions/{session_id}/start?kind=thermal").raise_for_status()

        body = b"\x01\x00" * 4
        self.client.post(
            "/api/v1/matrices/pressure?sequence=1&rows=2&cols=2",
            headers=self.headers,
            content=body,
        ).raise_for_status()
        self.client.post(
            "/api/v1/matrices/thermal?sequence=1&rows=2&cols=2",
            headers=other_headers,
            content=body,
        ).raise_for_status()
        self.client.post(
            "/api/v1/matrices/thermal?sequence=2&rows=2&cols=2",
            headers=self.headers,
            content=body,
        ).raise_for_status()

        samples = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0]["device_id"], "dev")
        self.assertEqual(samples[0]["kind"], "thermal")

    def test_dashboard_basic_auth_does_not_block_device_api(self) -> None:
        data = Path(self.temp.name) / "auth"
        root = Path(__file__).resolve().parents[1]
        settings = Settings(
            root, data, data / "db.sqlite3", data / "artifacts", {"dev": "secret"},
            admin_user="admin", admin_password="password",
        )
        with TestClient(create_app(settings)) as client:
            self.assertEqual(client.get("/").status_code, 401)
            self.assertEqual(client.get("/", auth=("admin", "password")).status_code, 200)
            response = client.post("/api/v1/telemetry", headers=self.headers, json={"sequence": 2})
            self.assertEqual(response.status_code, 200)

    def test_experiment_review_command_history_and_model_upload(self) -> None:
        self.client.post("/api/v1/device/hello", headers=self.headers, json={"firmware": "test"}).raise_for_status()
        created = self.client.post(
            "/api/dashboard/sessions",
            json={"device_id": "dev", "name": "pressure trial", "target_labels": ["empty", "occupied"]},
        ).json()
        session_id = created["session_id"]
        started = self.client.post(
            f"/api/dashboard/sessions/{session_id}/start?kind=pressure"
        ).json()
        self.assertIsNone(started["command_id"])

        self.client.post(
            "/api/v1/matrices/pressure?sequence=3&rows=2&cols=2",
            headers=self.headers,
            content=b"\x01\x00" * 4,
        ).raise_for_status()
        samples = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()
        self.assertEqual(len(samples), 1)
        self.client.patch(
            f"/api/dashboard/samples/{samples[0]['id']}",
            json={"label": "occupied", "quality": "good", "notes": "manual test"},
        ).raise_for_status()

        snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
        reviewed = self.client.get(f"/api/dashboard/sessions/{session_id}/samples").json()[0]
        self.assertEqual(reviewed["label"], "occupied")

        package = io.BytesIO()
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("manifest.json", '{"input":"pressure"}')
        uploaded = self.client.post(
            "/api/dashboard/models?name=pressure&version=1.0&target=ra8p1&task=classification",
            headers={"Content-Type": "application/zip"},
            content=package.getvalue(),
        )
        uploaded.raise_for_status()
        self.assertTrue(uploaded.json()["sha256"])
