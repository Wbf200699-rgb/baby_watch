from __future__ import annotations

import asyncio
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Awaitable, Callable


TELEMETRY_QUEUE_FRAMES = 2


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def _percentile_95(values: deque[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, (len(ordered) * 95 + 99) // 100 - 1))
    return round(ordered[index], 2)


@dataclass(frozen=True)
class TelemetryFrame:
    device_id: str
    ip_address: str
    sequence: int
    payload: dict[str, Any]
    received_at: str
    received_monotonic: float


@dataclass
class _TelemetryState:
    sequence: int | None = None
    received_at: str | None = None
    received_monotonic: float | None = None
    arrivals: deque[float] = field(default_factory=lambda: deque(maxlen=64))
    ack_ms: deque[float] = field(default_factory=lambda: deque(maxlen=256))
    database_ms: deque[float] = field(default_factory=lambda: deque(maxlen=128))
    received_frames: int = 0
    sequence_drops: int = 0
    publish_drops: int = 0
    persistence_drops: int = 0
    published_frames: int = 0
    persisted_frames: int = 0
    persistence_skips: int = 0
    publish_errors: int = 0
    persistence_errors: int = 0


PersistCallback = Callable[[TelemetryFrame], Awaitable[None]]
PublishCallback = Callable[[TelemetryFrame], Awaitable[None]]
CaptureActiveCallback = Callable[[str], Awaitable[bool]]


class TelemetryRealtimeBroker:
    """Latest-only telemetry path with ACK-independent bounded workers."""

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
        self._latest: dict[str, TelemetryFrame] = {}
        self._states: dict[str, _TelemetryState] = defaultdict(_TelemetryState)
        self._connections: dict[str, int] = defaultdict(int)
        self._connection_totals: dict[str, int] = defaultdict(int)
        self._reconnections: dict[str, int] = defaultdict(int)
        self._last_close_codes: dict[str, int | None] = {}
        self._last_connected_at: dict[str, str] = {}
        self._last_closed_at: dict[str, str] = {}
        self._publish_queues: dict[str, asyncio.Queue[TelemetryFrame]] = {}
        self._persistence_queues: dict[str, asyncio.Queue[TelemetryFrame]] = {}
        self._publish_tasks: dict[str, asyncio.Task[None]] = {}
        self._persistence_tasks: dict[str, asyncio.Task[None]] = {}
        self._last_persisted_at: dict[str, float] = {}
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

    def accept(self, frame: TelemetryFrame, *, missing_frames: int = 0) -> None:
        if self._closed:
            raise RuntimeError("telemetry broker is closed")
        with self._lock:
            state = self._states[frame.device_id]
            state.sequence = frame.sequence
            state.received_at = frame.received_at
            state.received_monotonic = frame.received_monotonic
            state.received_frames += 1
            state.sequence_drops += max(0, missing_frames)
            state.arrivals.append(frame.received_monotonic)
            self._latest[frame.device_id] = frame

        publish_queue = self._ensure_queue(frame.device_id, publish=True)
        persistence_queue = self._ensure_queue(frame.device_id, publish=False)
        self._put_latest(publish_queue, frame, publish=True)
        self._put_latest(persistence_queue, frame, publish=False)

    def record_ack(self, device_id: str, elapsed_ms: float) -> None:
        with self._lock:
            self._states[device_id].ack_ms.append(max(0.0, elapsed_ms))

    def latest(self, device_id: str) -> dict[str, Any] | None:
        with self._lock:
            frame = self._latest.get(device_id)
        if frame is None:
            return None
        return {**frame.payload, "received_at": frame.received_at}

    def _ensure_queue(
        self,
        device_id: str,
        *,
        publish: bool,
    ) -> asyncio.Queue[TelemetryFrame]:
        queues = self._publish_queues if publish else self._persistence_queues
        tasks = self._publish_tasks if publish else self._persistence_tasks
        queue = queues.get(device_id)
        if queue is None:
            queue = asyncio.Queue(maxsize=TELEMETRY_QUEUE_FRAMES)
            queues[device_id] = queue
            worker = self._publish_worker(device_id, queue) if publish else self._persistence_worker(device_id, queue)
            tasks[device_id] = asyncio.create_task(worker)
        return queue

    def _put_latest(
        self,
        queue: asyncio.Queue[TelemetryFrame],
        frame: TelemetryFrame,
        *,
        publish: bool,
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
                state = self._states[frame.device_id]
                if publish:
                    state.publish_drops += 1
                else:
                    state.persistence_drops += 1

    async def _publish_worker(
        self,
        device_id: str,
        queue: asyncio.Queue[TelemetryFrame],
    ) -> None:
        while True:
            frame = await queue.get()
            try:
                await self._publish(frame)
                with self._lock:
                    self._states[device_id].published_frames += 1
            except asyncio.CancelledError:
                raise
            except Exception:
                with self._lock:
                    self._states[device_id].publish_errors += 1
            finally:
                queue.task_done()

    async def _persistence_worker(
        self,
        device_id: str,
        queue: asyncio.Queue[TelemetryFrame],
    ) -> None:
        while True:
            frame = await queue.get()
            try:
                capture_active = await self._capture_active(device_id)
                now = time.monotonic()
                last_persisted = self._last_persisted_at.get(device_id, 0.0)
                if not capture_active and now - last_persisted < self._normal_persist_interval:
                    with self._lock:
                        self._states[device_id].persistence_skips += 1
                    continue
                started = time.perf_counter()
                await self._persist(frame)
                database_ms = (time.perf_counter() - started) * 1000.0
                self._last_persisted_at[device_id] = time.monotonic()
                with self._lock:
                    state = self._states[device_id]
                    state.persisted_frames += 1
                    state.database_ms.append(database_ms)
            except asyncio.CancelledError:
                raise
            except Exception:
                with self._lock:
                    self._states[device_id].persistence_errors += 1
            finally:
                queue.task_done()

    def status(self, device_id: str) -> dict[str, Any]:
        now = time.monotonic()
        with self._lock:
            state = self._states.get(device_id)
            connections = self._connections.get(device_id, 0)
            publish_queue = self._publish_queues.get(device_id)
            persistence_queue = self._persistence_queues.get(device_id)
            if state is None:
                return {
                    "telemetry_connected": connections > 0,
                    "telemetry_connections": connections,
                    "telemetry_fps": 0.0,
                    "telemetry_age_ms": None,
                    "telemetry_received_frames": 0,
                    "telemetry_ack_p95_ms": 0.0,
                    "telemetry_database_p95_ms": 0.0,
                    "telemetry_queue_depth": 0,
                    "telemetry_persistence_queue_depth": 0,
                    "telemetry_queue_limit": TELEMETRY_QUEUE_FRAMES,
                    "telemetry_drops": 0,
                    "telemetry_reconnects": self._reconnections.get(device_id, 0),
                    "telemetry_last_close_code": self._last_close_codes.get(device_id),
                }
            arrivals = [arrival for arrival in state.arrivals if now - arrival <= 5.0]
            fps = 0.0
            if len(arrivals) >= 2 and arrivals[-1] > arrivals[0]:
                fps = (len(arrivals) - 1) / (arrivals[-1] - arrivals[0])
            age_ms = (
                max(0, round((now - state.received_monotonic) * 1000))
                if state.received_monotonic is not None
                else None
            )
            return {
                "telemetry_connected": connections > 0,
                "telemetry_connections": connections,
                "telemetry_fps": round(fps, 2),
                "telemetry_age_ms": age_ms,
                "telemetry_received_frames": state.received_frames,
                "telemetry_sequence_drops": state.sequence_drops,
                "telemetry_publish_drops": state.publish_drops,
                "telemetry_persistence_drops": state.persistence_drops,
                "telemetry_drops": state.sequence_drops + max(state.publish_drops, state.persistence_drops),
                "telemetry_published_frames": state.published_frames,
                "telemetry_persisted_frames": state.persisted_frames,
                "telemetry_persistence_skips": state.persistence_skips,
                "telemetry_publish_errors": state.publish_errors,
                "telemetry_persistence_errors": state.persistence_errors,
                "telemetry_ack_p95_ms": _percentile_95(state.ack_ms),
                "telemetry_database_p95_ms": _percentile_95(state.database_ms),
                "telemetry_queue_depth": publish_queue.qsize() if publish_queue else 0,
                "telemetry_persistence_queue_depth": persistence_queue.qsize() if persistence_queue else 0,
                "telemetry_queue_limit": TELEMETRY_QUEUE_FRAMES,
                "telemetry_reconnects": self._reconnections.get(device_id, 0),
                "telemetry_last_close_code": self._last_close_codes.get(device_id),
                "telemetry_last_connected_at": self._last_connected_at.get(device_id),
                "telemetry_last_closed_at": self._last_closed_at.get(device_id),
            }

    async def close(self) -> None:
        self._closed = True
        tasks = tuple(self._publish_tasks.values()) + tuple(self._persistence_tasks.values())
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        self._publish_tasks.clear()
        self._persistence_tasks.clear()
