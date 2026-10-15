from __future__ import annotations

import asyncio
import struct
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Awaitable, Callable


MATRIX_HEADER = struct.Struct(">4sBBHHI")
MATRIX_MAGIC = b"RA8M"
MATRIX_VERSION = 1
MATRIX_MAX_FRAME_BYTES = 12 * 1024
MATRIX_QUEUE_FRAMES = 2
THERMAL_LEGACY_P4_DIMENSIONS = (80, 64)


def _percentile_95(values: deque[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, (len(ordered) * 95 + 99) // 100 - 1))
    return round(ordered[index], 2)


@dataclass(frozen=True)
class MatrixSpec:
    kind_id: int
    name: str
    rows: int
    cols: int
    unit: str


MATRIX_SPECS = {
    # Matrix metadata is rows x columns.  The sensor is conventionally named
    # 80x64 because it is 80 pixels wide and 64 pixels high.
    1: MatrixSpec(1, "thermal", 64, 80, "deci_c"),
    2: MatrixSpec(2, "pressure", 16, 16, "raw"),
}


class MatrixProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class MatrixFrame:
    device_id: str
    ip_address: str
    kind: str
    sequence: int
    rows: int
    cols: int
    unit: str
    payload: bytes
    received_at: str
    received_monotonic: float

    def values(self) -> list[int]:
        count = self.rows * self.cols
        return list(struct.unpack(f"<{count}h", self.payload))


@dataclass
class _KindState:
    sequence: int | None = None
    received_at: str | None = None
    received_monotonic: float | None = None
    arrivals: deque[float] = field(default_factory=lambda: deque(maxlen=64))
    ack_ms: deque[float] = field(default_factory=lambda: deque(maxlen=256))
    database_ms: deque[float] = field(default_factory=lambda: deque(maxlen=128))
    received_frames: int = 0
    sequence_drops: int = 0
    broadcast_drops: int = 0
    persistence_drops: int = 0
    broadcast_frames: int = 0
    persisted_frames: int = 0
    persistence_skips: int = 0
    persistence_errors: int = 0
    broadcast_errors: int = 0
    invalid_frames: int = 0


PersistCallback = Callable[[MatrixFrame], Awaitable[None]]
PublishCallback = Callable[[MatrixFrame], Awaitable[None]]
CaptureActiveCallback = Callable[[str, str], Awaitable[bool]]


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def parse_matrix_frame(
    raw: bytes,
    *,
    device_id: str,
    ip_address: str,
    strict_dimensions: bool = True,
) -> MatrixFrame:
    if len(raw) > MATRIX_MAX_FRAME_BYTES:
        raise MatrixProtocolError("matrix frame exceeds 12 KiB")
    if len(raw) < MATRIX_HEADER.size:
        raise MatrixProtocolError("matrix frame is shorter than its 14-byte header")

    magic, version, kind_id, rows, cols, sequence = MATRIX_HEADER.unpack_from(raw)
    if magic != MATRIX_MAGIC:
        raise MatrixProtocolError("invalid matrix magic")
    if version != MATRIX_VERSION:
        raise MatrixProtocolError("unsupported matrix protocol version")
    spec = MATRIX_SPECS.get(kind_id)
    if spec is None:
        raise MatrixProtocolError("unsupported matrix kind")
    if rows <= 0 or cols <= 0 or rows > 128 or cols > 128:
        raise MatrixProtocolError("invalid matrix dimensions")
    transmitted_rows = rows
    transmitted_cols = cols
    legacy_p4_thermal = (
        spec.name == "thermal" and (rows, cols) == THERMAL_LEGACY_P4_DIMENSIONS
    )
    if strict_dimensions and not (rows == spec.rows and cols == spec.cols) and not legacy_p4_thermal:
        raise MatrixProtocolError(
            f"{spec.name} matrix must be {spec.rows}x{spec.cols}, or legacy P4 "
            f"{THERMAL_LEGACY_P4_DIMENSIONS[0]}x{THERMAL_LEGACY_P4_DIMENSIONS[1]}, got "
            f"{rows}x{cols}"
        )

    expected_bytes = MATRIX_HEADER.size + transmitted_rows * transmitted_cols * 2
    if len(raw) != expected_bytes:
        raise MatrixProtocolError(
            f"matrix frame length mismatch: expected {expected_bytes}, got {len(raw)}"
        )

    if legacy_p4_thermal:
        # Gateway 4.2.0 labelled width and height as rows and columns. Its
        # payload is already row-major 64x80, so only metadata is normalized.
        rows, cols = spec.rows, spec.cols

    return MatrixFrame(
        device_id=device_id,
        ip_address=ip_address,
        kind=spec.name,
        sequence=sequence,
        rows=rows,
        cols=cols,
        unit=spec.unit,
        payload=bytes(raw[MATRIX_HEADER.size :]),
        received_at=utc_now(),
        received_monotonic=time.monotonic(),
    )


def sequence_gap(previous: int | None, current: int) -> tuple[int, bool]:
    """Return (missing frames, duplicate) and reject backward ordering."""
    if previous is None:
        return 0, False
    difference = (current - previous) & 0xFFFFFFFF
    if difference == 0:
        return 0, True
    if difference >= 0x80000000:
        raise MatrixProtocolError(
            f"out-of-order matrix sequence: previous={previous}, current={current}"
        )
    return difference - 1, False


class MatrixRealtimeBroker:
    """Latest-only matrix cache with independent bounded background paths."""

    def __init__(
        self,
        *,
        persist: PersistCallback,
        publish: PublishCallback,
        capture_active: CaptureActiveCallback,
        normal_persist_interval_seconds: float = 1.0,
    ) -> None:
        self._persist = persist
        self._publish = publish
        self._capture_active = capture_active
        self._normal_persist_interval = normal_persist_interval_seconds
        self._lock = threading.RLock()
        self._latest: dict[tuple[str, str], MatrixFrame] = {}
        self._states: dict[tuple[str, str], _KindState] = defaultdict(_KindState)
        self._connections: dict[str, int] = defaultdict(int)
        self._connection_totals: dict[str, int] = defaultdict(int)
        self._reconnections: dict[str, int] = defaultdict(int)
        self._last_close_codes: dict[str, int | None] = {}
        self._last_connected_at: dict[str, str] = {}
        self._last_closed_at: dict[str, str] = {}
        self._invalid_frames: dict[str, int] = defaultdict(int)
        self._last_invalid_errors: dict[str, str] = {}
        self._last_invalid_at: dict[str, str] = {}
        self._broadcast_queues: dict[tuple[str, str], asyncio.Queue[MatrixFrame]] = {}
        self._persistence_queues: dict[tuple[str, str], asyncio.Queue[MatrixFrame]] = {}
        self._broadcast_tasks: dict[tuple[str, str], asyncio.Task[None]] = {}
        self._persistence_tasks: dict[tuple[str, str], asyncio.Task[None]] = {}
        self._last_persisted_at: dict[tuple[str, str], float] = {}
        self._closed = False

    def connect(self, device_id: str) -> None:
        with self._lock:
            if self._connection_totals[device_id] > 0:
                self._reconnections[device_id] += 1
            self._connection_totals[device_id] += 1
            self._connections[device_id] += 1
            self._last_connected_at[device_id] = utc_now()

    def disconnect(self, device_id: str, close_code: int | None = None) -> None:
        with self._lock:
            remaining = max(0, self._connections.get(device_id, 0) - 1)
            if remaining:
                self._connections[device_id] = remaining
            else:
                self._connections.pop(device_id, None)
            self._last_close_codes[device_id] = close_code
            self._last_closed_at[device_id] = utc_now()

    def record_ack(self, device_id: str, kind: str, elapsed_ms: float) -> None:
        if kind not in {"pressure", "thermal"}:
            return
        with self._lock:
            self._states[(device_id, kind)].ack_ms.append(max(0.0, elapsed_ms))

    def note_invalid(self, device_id: str, message: str, kind: str | None = None) -> None:
        with self._lock:
            self._invalid_frames[device_id] += 1
            self._last_invalid_errors[device_id] = message
            self._last_invalid_at[device_id] = utc_now()
            if kind in {"pressure", "thermal"}:
                self._states[(device_id, kind)].invalid_frames += 1

    def accept(self, frame: MatrixFrame, *, missing_frames: int = 0) -> None:
        if self._closed:
            raise RuntimeError("matrix broker is closed")
        key = (frame.device_id, frame.kind)
        with self._lock:
            state = self._states[key]
            state.sequence = frame.sequence
            state.received_at = frame.received_at
            state.received_monotonic = frame.received_monotonic
            state.received_frames += 1
            state.sequence_drops += max(0, missing_frames)
            state.arrivals.append(frame.received_monotonic)
            self._latest[key] = frame

        broadcast_queue = self._ensure_queue(key, broadcast=True)
        persistence_queue = self._ensure_queue(key, broadcast=False)
        self._put_latest(broadcast_queue, frame, key=key, broadcast=True)
        self._put_latest(persistence_queue, frame, key=key, broadcast=False)

    def _ensure_queue(
        self,
        key: tuple[str, str],
        *,
        broadcast: bool,
    ) -> asyncio.Queue[MatrixFrame]:
        queues = self._broadcast_queues if broadcast else self._persistence_queues
        tasks = self._broadcast_tasks if broadcast else self._persistence_tasks
        queue = queues.get(key)
        if queue is None:
            queue = asyncio.Queue(maxsize=MATRIX_QUEUE_FRAMES)
            queues[key] = queue
            worker = self._broadcast_worker(key, queue) if broadcast else self._persistence_worker(key, queue)
            tasks[key] = asyncio.create_task(worker)
        return queue

    def _put_latest(
        self,
        queue: asyncio.Queue[MatrixFrame],
        frame: MatrixFrame,
        *,
        key: tuple[str, str],
        broadcast: bool,
    ) -> None:
        dropped = False
        if queue.full():
            try:
                queue.get_nowait()
                queue.task_done()
                dropped = True
            except asyncio.QueueEmpty:
                pass
        try:
            queue.put_nowait(frame)
        except asyncio.QueueFull:
            dropped = True
        if dropped:
            with self._lock:
                state = self._states[key]
                if broadcast:
                    state.broadcast_drops += 1
                else:
                    state.persistence_drops += 1

    async def _broadcast_worker(
        self,
        key: tuple[str, str],
        queue: asyncio.Queue[MatrixFrame],
    ) -> None:
        while True:
            frame = await queue.get()
            try:
                await self._publish(frame)
                with self._lock:
                    self._states[key].broadcast_frames += 1
            except asyncio.CancelledError:
                raise
            except Exception:
                with self._lock:
                    self._states[key].broadcast_errors += 1
            finally:
                queue.task_done()

    async def _persistence_worker(
        self,
        key: tuple[str, str],
        queue: asyncio.Queue[MatrixFrame],
    ) -> None:
        while True:
            frame = await queue.get()
            try:
                capture_active = await self._capture_active(frame.device_id, frame.kind)
                now = time.monotonic()
                last_persisted = self._last_persisted_at.get(key, 0.0)
                if not capture_active and now - last_persisted < self._normal_persist_interval:
                    with self._lock:
                        self._states[key].persistence_skips += 1
                    continue
                started = time.perf_counter()
                await self._persist(frame)
                database_ms = (time.perf_counter() - started) * 1000.0
                self._last_persisted_at[key] = time.monotonic()
                with self._lock:
                    state = self._states[key]
                    state.persisted_frames += 1
                    state.database_ms.append(database_ms)
            except asyncio.CancelledError:
                raise
            except Exception:
                with self._lock:
                    self._states[key].persistence_errors += 1
            finally:
                queue.task_done()

    def latest(self, device_id: str, kind: str) -> dict[str, object] | None:
        key = (device_id, kind)
        with self._lock:
            frame = self._latest.get(key)
        if frame is None:
            return None
        values = frame.values()
        result: dict[str, object] = {
            "kind": frame.kind,
            "sequence": frame.sequence,
            "rows": frame.rows,
            "cols": frame.cols,
            "unit": frame.unit,
            "received_at": frame.received_at,
            "minimum": min(values),
            "maximum": max(values),
            "values": values,
        }
        result.update(self.kind_status(device_id, kind))
        return result

    def kind_status(self, device_id: str, kind: str) -> dict[str, object]:
        key = (device_id, kind)
        now = time.monotonic()
        with self._lock:
            state = self._states.get(key)
            if state is None:
                return {
                    "fps": 0.0,
                    "frame_age_ms": None,
                    "drops": 0,
                    "delayed": True,
                }
            arrivals = [arrival for arrival in state.arrivals if now - arrival <= 5.0]
            age_ms = (
                max(0, round((now - state.received_monotonic) * 1000))
                if state.received_monotonic is not None
                else None
            )
            fps = 0.0
            if len(arrivals) >= 2 and arrivals[-1] > arrivals[0]:
                fps = (len(arrivals) - 1) / (arrivals[-1] - arrivals[0])
            drops = state.sequence_drops + max(state.broadcast_drops, state.persistence_drops)
            return {
                "fps": round(fps, 2),
                "frame_age_ms": age_ms,
                "drops": drops,
                "delayed": age_ms is None or age_ms > 2000,
                "received_frames": state.received_frames,
                "sequence_drops": state.sequence_drops,
                "broadcast_drops": state.broadcast_drops,
                "persistence_drops": state.persistence_drops,
                "persisted_frames": state.persisted_frames,
                "persistence_skips": state.persistence_skips,
                "persistence_errors": state.persistence_errors,
                "broadcast_errors": state.broadcast_errors,
                "invalid_frames": state.invalid_frames,
                "ack_p95_ms": _percentile_95(state.ack_ms),
                "database_p95_ms": _percentile_95(state.database_ms),
            }

    def status(self, device_id: str) -> dict[str, object]:
        pressure = self.kind_status(device_id, "pressure")
        thermal = self.kind_status(device_id, "thermal")
        with self._lock:
            pressure_state = self._states.get((device_id, "pressure"))
            thermal_state = self._states.get((device_id, "thermal"))
            connections = self._connections.get(device_id, 0)
            pressure_queue = self._broadcast_queues.get((device_id, "pressure"))
            thermal_queue = self._broadcast_queues.get((device_id, "thermal"))
            pressure_persistence_queue = self._persistence_queues.get((device_id, "pressure"))
            thermal_persistence_queue = self._persistence_queues.get((device_id, "thermal"))
        return {
            "matrix_connected": connections > 0,
            "matrix_connections": connections,
            "last_pressure_at": pressure_state.received_at if pressure_state else None,
            "last_thermal_at": thermal_state.received_at if thermal_state else None,
            "pressure_fps": pressure["fps"],
            "thermal_fps": thermal["fps"],
            "pressure_age_ms": pressure["frame_age_ms"],
            "thermal_age_ms": thermal["frame_age_ms"],
            "pressure_drops": pressure["drops"],
            "thermal_drops": thermal["drops"],
            "matrix_drops": int(pressure["drops"]) + int(thermal["drops"]),
            "pressure_received_frames": pressure.get("received_frames", 0),
            "thermal_received_frames": thermal.get("received_frames", 0),
            "pressure_queue_depth": pressure_queue.qsize() if pressure_queue else 0,
            "thermal_queue_depth": thermal_queue.qsize() if thermal_queue else 0,
            "pressure_persistence_queue_depth": (
                pressure_persistence_queue.qsize() if pressure_persistence_queue else 0
            ),
            "thermal_persistence_queue_depth": (
                thermal_persistence_queue.qsize() if thermal_persistence_queue else 0
            ),
            "pressure_ack_p95_ms": pressure.get("ack_p95_ms", 0.0),
            "thermal_ack_p95_ms": thermal.get("ack_p95_ms", 0.0),
            "pressure_database_p95_ms": pressure.get("database_p95_ms", 0.0),
            "thermal_database_p95_ms": thermal.get("database_p95_ms", 0.0),
            "matrix_reconnects": self._reconnections.get(device_id, 0),
            "matrix_last_close_code": self._last_close_codes.get(device_id),
            "matrix_last_connected_at": self._last_connected_at.get(device_id),
            "matrix_last_closed_at": self._last_closed_at.get(device_id),
            "matrix_invalid_frames": self._invalid_frames.get(device_id, 0),
            "matrix_last_invalid_error": self._last_invalid_errors.get(device_id),
            "matrix_last_invalid_at": self._last_invalid_at.get(device_id),
            "queue_limit": MATRIX_QUEUE_FRAMES,
        }

    async def close(self) -> None:
        self._closed = True
        tasks = tuple(self._broadcast_tasks.values()) + tuple(self._persistence_tasks.values())
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        self._broadcast_tasks.clear()
        self._persistence_tasks.clear()
