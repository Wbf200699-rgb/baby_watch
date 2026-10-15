import asyncio
import sqlite3
import sys
import time
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import IsolatedAsyncioTestCase, TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app
from app.telemetry_stream import TelemetryFrame, TelemetryRealtimeBroker


def telemetry_frame(sequence: int) -> TelemetryFrame:
    now = time.monotonic()
    return TelemetryFrame(
        device_id="dev",
        ip_address="127.0.0.1",
        sequence=sequence,
        payload={"sequence": sequence, "sensors": {}, "health": {}},
        received_at=f"2026-07-30T00:00:{sequence % 60:02d}.000+00:00",
        received_monotonic=now,
    )


class TelemetryStreamApiTest(TestCase):
    def setUp(self) -> None:
        self.temp = TemporaryDirectory()
        data = Path(self.temp.name)
        root = Path(__file__).resolve().parents[1]
        settings = Settings(
            root,
            data,
            data / "db.sqlite3",
            data / "artifacts",
            {"dev": "secret"},
        )
        self.client_context = TestClient(create_app(settings))
        self.client = self.client_context.__enter__()
        self.headers = {"X-Device-ID": "dev", "X-Device-Key": "secret"}
        self.client.post(
            "/api/v1/device/hello",
            headers=self.headers,
            json={"firmware": "p4x-test", "hardware": "ESP32-P4X-EYE"},
        ).raise_for_status()

    def tearDown(self) -> None:
        self.client_context.__exit__(None, None, None)
        self.temp.cleanup()

    def test_slow_database_does_not_delay_ack_or_latest_snapshot(self) -> None:
        async def slow_persist(_frame) -> None:
            await asyncio.sleep(0.5)

        self.client.app.state.telemetry_runtime._persist = slow_persist
        with self.client.websocket_connect("/api/v1/device/stream", headers=self.headers) as websocket:
            started = time.perf_counter()
            websocket.send_json({"kind": "telemetry", "payload": {"sequence": 700}})
            ack = websocket.receive_json()
            elapsed = time.perf_counter() - started
            snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
            status = self.client.get("/api/dashboard/devices/dev/transport-status").json()["telemetry"]

        self.assertEqual(ack["kind"], "telemetry_ack")
        self.assertEqual(ack["payload"]["sequence"], 700)
        self.assertLess(elapsed, 0.2)
        self.assertEqual(snapshot["telemetry"]["sequence"], 700)
        self.assertEqual(status["telemetry_received_frames"], 1)
        self.assertGreaterEqual(status["telemetry_ack_p95_ms"], 0.0)

    def test_first_sequence_is_arbitrary_and_reconnect_can_reset_sequence(self) -> None:
        for sequence in (0xF0000000, 3):
            with self.client.websocket_connect("/api/v1/device/stream", headers=self.headers) as websocket:
                websocket.send_json({"kind": "telemetry", "payload": {"sequence": sequence}})
                self.assertEqual(websocket.receive_json()["payload"]["sequence"], sequence)
        status = self.client.get("/api/dashboard/devices/dev/transport-status").json()["telemetry"]
        self.assertGreaterEqual(status["telemetry_reconnects"], 1)

    def test_command_database_failure_does_not_close_telemetry_stream(self) -> None:
        database = self.client.app.state.db
        original_claim = database.claim_command

        def fail_claim(*_args, **_kwargs):
            raise sqlite3.OperationalError("database is temporarily busy")

        database.claim_command = fail_claim
        try:
            with self.client.websocket_connect("/api/v1/device/stream", headers=self.headers) as websocket:
                websocket.send_json({"kind": "hello", "payload": {"firmware": "p4x-test"}})
                self.assertEqual(websocket.receive_json()["kind"], "hello_ack")
                time.sleep(0.7)
                websocket.send_json({"kind": "heartbeat", "payload": {}})
                self.assertEqual(websocket.receive_json()["kind"], "heartbeat_ack")
        finally:
            database.claim_command = original_claim


class TelemetryRealtimeBrokerTest(IsolatedAsyncioTestCase):
    async def test_slow_workers_keep_only_two_latest_frames(self) -> None:
        release = asyncio.Event()

        async def slow(_frame) -> None:
            await release.wait()

        async def inactive(_device_id: str) -> bool:
            return False

        broker = TelemetryRealtimeBroker(persist=slow, publish=slow, capture_active=inactive)
        try:
            for sequence in range(10):
                broker.accept(telemetry_frame(sequence))
            await asyncio.sleep(0)
            status = broker.status("dev")
            self.assertLessEqual(status["telemetry_queue_depth"], 2)
            self.assertLessEqual(status["telemetry_persistence_queue_depth"], 2)
            self.assertGreater(status["telemetry_drops"], 0)
            self.assertEqual(broker.latest("dev")["sequence"], 9)
        finally:
            release.set()
            await broker.close()
