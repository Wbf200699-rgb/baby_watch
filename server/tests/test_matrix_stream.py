import asyncio
import struct
import sys
import time
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import IsolatedAsyncioTestCase, TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.config import Settings
from app.main import create_app
from app.matrix_stream import (
    MATRIX_HEADER,
    MatrixRealtimeBroker,
    parse_matrix_frame,
)


def matrix_frame(kind_id: int, rows: int, cols: int, sequence: int, value: int = 0) -> bytes:
    payload = struct.pack(f"<{rows * cols}h", *([value] * (rows * cols)))
    return MATRIX_HEADER.pack(b"RA8M", 1, kind_id, rows, cols, sequence) + payload


class MatrixStreamApiTest(TestCase):
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
        self.client.post(
            "/api/v1/device/hello",
            headers=self.headers,
            json={"firmware": "matrix-test"},
        ).raise_for_status()

    def tearDown(self) -> None:
        self.client_context.__exit__(None, None, None)
        self.temp.cleanup()

    def test_pressure_and_thermal_frames_are_acked_and_cached(self) -> None:
        with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as websocket:
            websocket.send_bytes(matrix_frame(2, 16, 16, 10, 123))
            pressure_ack = websocket.receive_json()
            self.assertEqual(pressure_ack["kind"], "matrix_ack")
            self.assertEqual(pressure_ack["payload"]["kind"], "pressure")
            self.assertEqual(pressure_ack["payload"]["sequence"], 10)

            websocket.send_bytes(matrix_frame(1, 64, 80, 20, 245))
            thermal_ack = websocket.receive_json()
            self.assertEqual(thermal_ack["payload"]["kind"], "thermal")
            self.assertEqual(thermal_ack["payload"]["sequence"], 20)

            status = self.client.get("/api/dashboard/devices/dev/matrix-status").json()
            self.assertTrue(status["matrix_connected"])
            self.assertEqual(status["pressure_received_frames"], 1)
            self.assertEqual(status["thermal_received_frames"], 1)
            self.assertGreaterEqual(status["pressure_ack_p95_ms"], 0.0)
            self.assertGreaterEqual(status["thermal_ack_p95_ms"], 0.0)

            snapshot = self.client.get("/api/dashboard/snapshot?device_id=dev").json()
            self.assertEqual(snapshot["matrices"]["pressure"]["values"], [123] * 256)
            self.assertEqual(snapshot["matrices"]["thermal"]["values"], [245] * 5120)
            self.assertEqual(snapshot["matrices"]["thermal"]["rows"], 64)
            self.assertEqual(snapshot["matrices"]["thermal"]["cols"], 80)

        status = self.client.get("/api/dashboard/devices/dev/matrix-status").json()
        self.assertFalse(status["matrix_connected"])

    def test_legacy_p4_thermal_dimensions_remain_compatible(self) -> None:
        values = list(range(5120))
        payload = struct.pack("<5120h", *values)
        previous = MATRIX_HEADER.pack(b"RA8M", 1, 1, 80, 64, 7) + payload
        with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as websocket:
            websocket.send_bytes(previous)
            self.assertEqual(websocket.receive_json()["payload"]["sequence"], 7)

        matrix = self.client.get("/api/dashboard/snapshot?device_id=dev").json()["matrices"]["thermal"]
        self.assertEqual((matrix["rows"], matrix["cols"]), (64, 80))
        self.assertEqual(matrix["values"], values)

    def test_canonical_thermal_coordinates_are_not_transposed(self) -> None:
        values = [0] * 5120
        values[0] = 101
        values[79] = 202
        values[63 * 80] = 303
        values[63 * 80 + 79] = 404
        payload = struct.pack("<5120h", *values)
        canonical = MATRIX_HEADER.pack(b"RA8M", 1, 1, 64, 80, 8) + payload
        frame = parse_matrix_frame(
            canonical,
            device_id="dev",
            ip_address="127.0.0.1",
        )
        self.assertEqual((frame.rows, frame.cols), (64, 80))
        decoded = frame.values()
        self.assertEqual(
            (decoded[0], decoded[79], decoded[63 * 80], decoded[-1]),
            (101, 202, 303, 404),
        )

    def test_invalid_credentials_are_rejected(self) -> None:
        with self.assertRaises(WebSocketDisconnect) as context:
            with self.client.websocket_connect(
                "/api/v1/device/matrices",
                headers={"X-Device-ID": "dev", "X-Device-Key": "wrong"},
            ):
                pass
        self.assertEqual(context.exception.code, 1008)

    def test_invalid_magic_version_dimensions_and_length_are_rejected(self) -> None:
        invalid_frames = [
            MATRIX_HEADER.pack(b"NOPE", 1, 2, 16, 16, 1) + bytes(512),
            MATRIX_HEADER.pack(b"RA8M", 2, 2, 16, 16, 1) + bytes(512),
            matrix_frame(2, 8, 8, 1),
            MATRIX_HEADER.pack(b"RA8M", 1, 2, 16, 16, 1) + bytes(510),
        ]
        for frame in invalid_frames:
            with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as websocket:
                websocket.send_bytes(frame)
                error = websocket.receive_json()
                self.assertEqual(error["kind"], "error")
                closed = websocket.receive()
                self.assertEqual(closed["type"], "websocket.close")
                self.assertEqual(closed["code"], 1003)
        status = self.client.get("/api/dashboard/devices/dev/matrix-status").json()
        self.assertEqual(status["matrix_invalid_frames"], len(invalid_frames))
        self.assertTrue(status["matrix_last_invalid_error"])

    def test_telemetry_and_matrix_connections_coexist(self) -> None:
        with self.client.websocket_connect("/api/v1/device/stream", headers=self.headers) as telemetry:
            with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as matrices:
                telemetry.send_json({"kind": "telemetry", "payload": {"sequence": 41}})
                self.assertEqual(telemetry.receive_json()["kind"], "telemetry_ack")
                matrices.send_bytes(matrix_frame(2, 16, 16, 42, 7))
                self.assertEqual(matrices.receive_json()["payload"]["sequence"], 42)
                telemetry.send_json({"kind": "heartbeat", "payload": {}})
                self.assertEqual(telemetry.receive_json()["kind"], "heartbeat_ack")

    def test_slow_database_does_not_delay_ack(self) -> None:
        async def slow_persist(_frame) -> None:
            await asyncio.sleep(0.5)

        self.client.app.state.matrix_runtime._persist = slow_persist
        with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as websocket:
            started = time.perf_counter()
            websocket.send_bytes(matrix_frame(2, 16, 16, 1, 9))
            ack = websocket.receive_json()
            elapsed = time.perf_counter() - started
        self.assertEqual(ack["kind"], "matrix_ack")
        self.assertLess(elapsed, 0.2)

    def test_connection_can_reconnect_with_reset_sequence(self) -> None:
        with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as websocket:
            websocket.send_bytes(matrix_frame(2, 16, 16, 100, 1))
            self.assertEqual(websocket.receive_json()["payload"]["sequence"], 100)
        with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as websocket:
            websocket.send_bytes(matrix_frame(2, 16, 16, 1, 2))
            self.assertEqual(websocket.receive_json()["payload"]["sequence"], 1)
        status = self.client.get("/api/dashboard/devices/dev/matrix-status").json()
        self.assertGreaterEqual(status["matrix_reconnects"], 1)

    def test_duplicate_is_idempotently_acked_and_gap_is_counted(self) -> None:
        with self.client.websocket_connect("/api/v1/device/matrices", headers=self.headers) as websocket:
            for sequence in (1, 1, 4):
                websocket.send_bytes(matrix_frame(2, 16, 16, sequence, sequence))
                self.assertEqual(websocket.receive_json()["payload"]["sequence"], sequence)
        status = self.client.get("/api/dashboard/devices/dev/matrix-status").json()
        self.assertEqual(status["pressure_received_frames"], 2)
        self.assertEqual(status["pressure_drops"], 2)


class MatrixRealtimeBrokerTest(IsolatedAsyncioTestCase):
    async def test_slow_consumers_keep_only_two_latest_frames(self) -> None:
        release = asyncio.Event()

        async def slow(_frame) -> None:
            await release.wait()

        async def inactive(_device_id: str, _kind: str) -> bool:
            return False

        broker = MatrixRealtimeBroker(persist=slow, publish=slow, capture_active=inactive)
        try:
            for sequence in range(10):
                frame = parse_matrix_frame(
                    matrix_frame(2, 16, 16, sequence, sequence),
                    device_id="dev",
                    ip_address="127.0.0.1",
                )
                broker.accept(frame)
            await asyncio.sleep(0)
            status = broker.status("dev")
            self.assertLessEqual(status["pressure_queue_depth"], 2)
            self.assertGreater(status["pressure_drops"], 0)
            self.assertEqual(broker.latest("dev", "pressure")["sequence"], 9)
        finally:
            release.set()
            await broker.close()
