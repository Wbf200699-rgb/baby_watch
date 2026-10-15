from __future__ import annotations

import argparse
import json
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

from ra8_protocol import (
    FrameParser,
    TYPE_ALARM_UPLOAD,
    TYPE_CMD,
    TYPE_PRESSURE_UPLOAD,
    TYPE_SAFETY_ACK,
    TYPE_SAFETY_EVENT,
    TYPE_STATUS_UPLOAD,
    decode_pressure_chunk,
    decode_thermal_chunk,
    encode_frame,
)


def require_serial():
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("Missing dependency: pip install pyserial") from exc
    return serial


class MatrixReassembler:
    def __init__(self, decoder, scale: float = 1.0) -> None:
        self.decoder = decoder
        self.scale = scale
        self.sequence: int | None = None
        self.width = 0
        self.height = 0
        self.samples: list[float | None] = []
        self.valid: list[bool] = []
        self.completed_count = 0
        self.updated_at = 0.0

    def add_chunk(self, payload: bytes) -> list[list[float | None]] | None:
        sequence, width, height, offset, samples = self.decoder(payload)
        total = width * height

        if self.sequence != sequence or len(self.samples) != total:
            self.sequence = sequence
            self.width = width
            self.height = height
            self.samples = [None] * total
            self.valid = [False] * total

        end = min(offset + len(samples), total)
        if end <= offset:
            return None

        for index, sample in enumerate(samples[: end - offset], start=offset):
            self.samples[index] = float(sample) * self.scale
            self.valid[index] = True

        if all(self.valid):
            self.valid = [False] * total
            self.completed_count += 1
            self.updated_at = time.time()
            return self.matrix()

        return None

    def matrix(self) -> list[list[float | None]] | None:
        if self.width <= 0 or self.height <= 0 or len(self.samples) != self.width * self.height:
            return None

        return [
            self.samples[row * self.width : (row + 1) * self.width]
            for row in range(self.height)
        ]


class BridgeState:
    def __init__(self, history_limit: int) -> None:
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.started_at = time.time()
        self.latest_status: dict[str, Any] | None = None
        self.latest_thermal: list[list[float | None]] | None = None
        self.latest_pressure: list[list[float | None]] | None = None
        self.status_updated_at = 0.0
        self.frame_count = 0
        self.status_count = 0
        self.thermal_count = 0
        self.pressure_count = 0
        self.safety_event_count = 0
        self.safety_ack_count = 0
        self.json_error_count = 0
        self.serial_error: str | None = None
        self.parser_bad_crc = 0
        self.parser_bad_length = 0
        self.history: deque[dict[str, Any]] = deque(maxlen=history_limit)
        self.command_sequence = 1
        self.serial_handle = None
        self.serial_write_lock = threading.Lock()

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            now = time.time()
            return {
                "bridge": {
                    "uptime_s": round(now - self.started_at, 3),
                    "frame_count": self.frame_count,
                    "status_count": self.status_count,
                    "thermal_count": self.thermal_count,
                    "pressure_count": self.pressure_count,
                    "safety_event_count": self.safety_event_count,
                    "safety_ack_count": self.safety_ack_count,
                    "json_error_count": self.json_error_count,
                    "parser_bad_crc": self.parser_bad_crc,
                    "parser_bad_length": self.parser_bad_length,
                    "serial_error": self.serial_error,
                    "status_age_s": None if self.status_updated_at == 0.0 else round(now - self.status_updated_at, 3),
                },
                "status": self.latest_status,
                "thermal": self.latest_thermal,
                "pressure": self.latest_pressure,
            }

    def publish(self, event: dict[str, Any]) -> None:
        with self.cond:
            self.history.append(event)
            self.cond.notify_all()

    def send_frame(self, msg_type: int, payload: bytes) -> bool:
        with self.lock:
            ser = self.serial_handle
            sequence = self.command_sequence
            self.command_sequence = (self.command_sequence + 1) & 0xFFFF

        if ser is None:
            return False

        try:
            with self.serial_write_lock:
                ser.write(encode_frame(msg_type, payload, sequence=sequence))
            return True
        except Exception as exc:
            with self.lock:
                self.serial_error = str(exc)
            return False

    def send_command(self, payload: bytes) -> bool:
        return self.send_frame(TYPE_CMD, payload)


def serial_worker(state: BridgeState, port: str, baud: int, reconnect_s: float) -> None:
    serial = require_serial()
    parser = FrameParser(max_payload=4096)
    thermal = MatrixReassembler(decode_thermal_chunk, scale=0.1)
    pressure = MatrixReassembler(decode_pressure_chunk, scale=1.0)

    while True:
        try:
            with serial.Serial(port, baud, timeout=0.1) as ser:
                with state.lock:
                    state.serial_handle = ser
                    state.serial_error = None

                while True:
                    data = ser.read(4096)
                    if not data:
                        continue

                    for frame in parser.feed(data):
                        event: dict[str, Any] | None = None
                        with state.lock:
                            state.frame_count += 1
                            state.parser_bad_crc = parser.bad_crc_count
                            state.parser_bad_length = parser.bad_length_count

                        if frame.msg_type == TYPE_STATUS_UPLOAD:
                            try:
                                status = json.loads(frame.payload.decode("utf-8"))
                            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                                with state.lock:
                                    state.json_error_count += 1
                                event = {"type": "json_error", "error": str(exc), "time": time.time()}
                            else:
                                with state.lock:
                                    state.latest_status = status
                                    state.status_updated_at = time.time()
                                    state.status_count += 1
                                event = {"type": "status", "data": status, "time": time.time()}

                        elif frame.msg_type == TYPE_ALARM_UPLOAD:
                            matrix = thermal.add_chunk(frame.payload)
                            if matrix is not None:
                                with state.lock:
                                    state.latest_thermal = matrix
                                    state.thermal_count += 1
                                event = {"type": "thermal", "data": matrix, "time": time.time()}

                        elif frame.msg_type == TYPE_PRESSURE_UPLOAD:
                            matrix = pressure.add_chunk(frame.payload)
                            if matrix is not None:
                                with state.lock:
                                    state.latest_pressure = matrix
                                    state.pressure_count += 1
                                event = {"type": "pressure", "data": matrix, "time": time.time()}

                        elif frame.msg_type == TYPE_SAFETY_EVENT:
                            try:
                                safety = json.loads(frame.payload.decode("utf-8"))
                                event_id = int(safety.get("event", 0))
                            except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError) as exc:
                                with state.lock:
                                    state.json_error_count += 1
                                event = {"type": "json_error", "error": str(exc), "time": time.time()}
                            else:
                                acknowledged = event_id > 0 and state.send_frame(
                                    TYPE_SAFETY_ACK,
                                    event_id.to_bytes(4, "big"),
                                )
                                with state.lock:
                                    state.safety_event_count += 1
                                    if acknowledged:
                                        state.safety_ack_count += 1
                                event = {
                                    "type": "safety",
                                    "data": safety,
                                    "acknowledged": acknowledged,
                                    "time": time.time(),
                                }

                        if event is not None:
                            state.publish(event)

        except Exception as exc:
            with state.lock:
                state.serial_handle = None
                state.serial_error = str(exc)
            state.publish({"type": "serial_error", "error": str(exc), "time": time.time()})
            time.sleep(reconnect_s)


class BridgeHandler(BaseHTTPRequestHandler):
    server_version = "RA8SerialBridge/1.0"

    def _json(self, value: Any, code: int = 200) -> None:
        payload = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(payload)

    @property
    def state(self) -> BridgeState:
        return self.server.state  # type: ignore[attr-defined]

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"

        if path == "/":
            self._json({
                "name": "RA8 serial bridge",
                "endpoints": {
                    "/status": "latest parsed status plus bridge counters",
                    "/pressure": "latest pressure matrix",
                    "/thermal": "latest thermal matrix",
                    "/history?limit=20": "recent events",
                    "/stream": "server-sent event stream",
                    "/cmd?text=play_boot": "send a protocol command payload",
                },
            })
        elif path == "/status":
            self._json(self.state.snapshot())
        elif path == "/pressure":
            snapshot = self.state.snapshot()
            self._json({"pressure": snapshot["pressure"], "prs": (snapshot["status"] or {}).get("prs", {})})
        elif path == "/thermal":
            snapshot = self.state.snapshot()
            self._json({"thermal": snapshot["thermal"], "thm": (snapshot["status"] or {}).get("thm", {})})
        elif path == "/history":
            limit_text = parse_qs(parsed.query).get("limit", ["50"])[0]
            try:
                limit = max(1, min(500, int(limit_text)))
            except ValueError:
                limit = 50
            with self.state.lock:
                items = list(self.state.history)[-limit:]
            self._json({"events": items})
        elif path == "/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            last_index = 0
            while True:
                with self.state.cond:
                    self.state.cond.wait(timeout=5.0)
                    events = list(self.state.history)
                new_events = events[last_index:]
                last_index = len(events)
                if not new_events:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    continue
                for event in new_events:
                    payload = json.dumps(event, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
                    self.wfile.write(b"data: " + payload + b"\n\n")
                    self.wfile.flush()
        elif path == "/cmd":
            text = parse_qs(parsed.query).get("text", [""])[0]
            if not text:
                self._json({"ok": False, "error": "missing text query"}, code=400)
            else:
                ok = self.state.send_command(text.encode("utf-8"))
                self._json({"ok": ok, "text": text})
        else:
            self._json({"ok": False, "error": "not found"}, code=404)

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path.rstrip("/") != "/cmd":
            self._json({"ok": False, "error": "not found"}, code=404)
            return

        length = int(self.headers.get("Content-Length", "0"))
        payload = self.rfile.read(min(length, 4096))
        ok = self.state.send_command(payload)
        self._json({"ok": ok, "bytes": len(payload)})

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"{self.client_address[0]} - {fmt % args}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Expose one local RA8 serial port to LAN clients over HTTP")
    parser.add_argument("--port", required=True, help="Local serial port, e.g. COM21")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind address")
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--history", type=int, default=200)
    parser.add_argument("--reconnect-s", type=float, default=2.0)
    args = parser.parse_args()

    state = BridgeState(history_limit=args.history)
    worker = threading.Thread(
        target=serial_worker,
        args=(state, args.port, args.baud, args.reconnect_s),
        daemon=True,
    )
    worker.start()

    server = ThreadingHTTPServer((args.host, args.http_port), BridgeHandler)
    server.state = state  # type: ignore[attr-defined]

    print(f"serial bridge reading {args.port} @ {args.baud}")
    print(f"HTTP listening on http://{args.host}:{args.http_port}/")
    print("LAN clients can open /status, /pressure, /thermal, /stream")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("stopped")
    finally:
        server.server_close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
