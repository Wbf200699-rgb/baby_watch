from __future__ import annotations

import asyncio
import hmac
import struct
import time
from collections import defaultdict, deque
from dataclasses import dataclass
from datetime import datetime, timezone
from collections.abc import AsyncIterator
from typing import Any

from fastapi import HTTPException, Request, status

from .config import Settings


MATRIX_KINDS = {"thermal", "pressure"}
S3_MEDIA_MAGIC = b"S3MD"
S3_MEDIA_VERSION = 1
S3_MEDIA_TYPE_JPEG = 1
S3_MEDIA_TYPE_PCM16LE = 2
S3_MEDIA_FLAG_DISCONTINUITY = 1 << 0
S3_MEDIA_HEADER = struct.Struct(">4sBBHIQIHHHH")
S3_AUDIO_SAMPLE_RATE = 16000
S3_AUDIO_SAMPLES_PER_FRAME = 320
S3_AUDIO_CHANNELS = 1
S3_AUDIO_BITS_PER_SAMPLE = 16
S3_AUDIO_PAYLOAD_BYTES = 640
S3_AUDIO_SUBSCRIBER_QUEUE_FRAMES = 8
S3_VIDEO_MAX_PAYLOAD_BYTES = 512 * 1024
S3_VIDEO_SUBSCRIBER_QUEUE_FRAMES = 1
S3_VIDEO_MIN_TARGET_FPS = 8
S3_VIDEO_MAX_TARGET_FPS = 12
S3_VIDEO_TARGET_FPS = 8
S3_MEDIA_CONTROL_REFRESH_SECONDS = 10.0


class MediaProtocolError(ValueError):
    """Raised when a device sends a malformed S3MD media frame."""


@dataclass(frozen=True)
class S3AudioFrame:
    sequence: int
    timestamp_us: int
    flags: int
    sample_rate: int
    samples_per_frame: int
    channels: int
    bits_per_sample: int
    received_at: str
    payload: bytes
    wire_data: bytes


@dataclass(frozen=True)
class S3VideoFrame:
    sequence: int
    timestamp_us: int
    flags: int
    width: int
    height: int
    quality: int
    received_at: str
    payload: bytes
    wire_data: bytes


def decode_s3_audio_frame(data: bytes) -> S3AudioFrame:
    if len(data) < S3_MEDIA_HEADER.size:
        raise MediaProtocolError("S3MD frame is shorter than its 32-byte header")

    (
        magic,
        version,
        media_type,
        flags,
        sequence,
        timestamp_us,
        payload_length,
        sample_rate,
        samples_per_frame,
        channels,
        bits_per_sample,
    ) = S3_MEDIA_HEADER.unpack_from(data)

    if magic != S3_MEDIA_MAGIC:
        raise MediaProtocolError("invalid S3MD magic")
    if version != S3_MEDIA_VERSION:
        raise MediaProtocolError(f"unsupported S3MD version: {version}")
    if media_type != S3_MEDIA_TYPE_PCM16LE:
        raise MediaProtocolError("audio endpoint only accepts PCM16LE media_type=2")
    if flags & ~S3_MEDIA_FLAG_DISCONTINUITY:
        raise MediaProtocolError("unsupported S3MD audio flags")
    if payload_length != S3_AUDIO_PAYLOAD_BYTES:
        raise MediaProtocolError("PCM payload must be exactly 640 bytes")
    if len(data) != S3_MEDIA_HEADER.size + payload_length:
        raise MediaProtocolError("S3MD payload length does not match the WebSocket message")
    if (
        sample_rate != S3_AUDIO_SAMPLE_RATE
        or samples_per_frame != S3_AUDIO_SAMPLES_PER_FRAME
        or channels != S3_AUDIO_CHANNELS
        or bits_per_sample != S3_AUDIO_BITS_PER_SAMPLE
    ):
        raise MediaProtocolError("PCM format must be 16 kHz mono PCM16 with 320 samples per frame")

    return S3AudioFrame(
        sequence=sequence,
        timestamp_us=timestamp_us,
        flags=flags,
        sample_rate=sample_rate,
        samples_per_frame=samples_per_frame,
        channels=channels,
        bits_per_sample=bits_per_sample,
        received_at=datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        payload=data[S3_MEDIA_HEADER.size :],
        wire_data=data,
    )


def decode_s3_video_frame(data: bytes) -> S3VideoFrame:
    if len(data) < S3_MEDIA_HEADER.size:
        raise MediaProtocolError("S3MD frame is shorter than its 32-byte header")

    (
        magic,
        version,
        media_type,
        flags,
        sequence,
        timestamp_us,
        payload_length,
        width,
        height,
        quality,
        reserved,
    ) = S3_MEDIA_HEADER.unpack_from(data)

    if magic != S3_MEDIA_MAGIC:
        raise MediaProtocolError("invalid S3MD magic")
    if version != S3_MEDIA_VERSION:
        raise MediaProtocolError(f"unsupported S3MD version: {version}")
    if media_type != S3_MEDIA_TYPE_JPEG:
        raise MediaProtocolError("video endpoint only accepts JPEG media_type=1")
    if flags & ~S3_MEDIA_FLAG_DISCONTINUITY:
        raise MediaProtocolError("unsupported S3MD video flags")
    if not 0 < payload_length <= S3_VIDEO_MAX_PAYLOAD_BYTES:
        raise MediaProtocolError("JPEG payload is empty or larger than 512 KiB")
    if len(data) != S3_MEDIA_HEADER.size + payload_length:
        raise MediaProtocolError("S3MD payload length does not match the WebSocket message")
    if not 0 < width <= 4096 or not 0 < height <= 4096:
        raise MediaProtocolError("invalid JPEG dimensions")
    if quality > 63 or reserved != 0:
        raise MediaProtocolError("invalid JPEG metadata")

    payload = data[S3_MEDIA_HEADER.size :]
    if not payload.startswith(b"\xff\xd8") or not payload.endswith(b"\xff\xd9"):
        raise MediaProtocolError("JPEG payload is missing SOI or EOI marker")

    return S3VideoFrame(
        sequence=sequence,
        timestamp_us=timestamp_us,
        flags=flags,
        width=width,
        height=height,
        quality=quality,
        received_at=datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        payload=payload,
        wire_data=data,
    )


@dataclass(frozen=True)
class VideoFrame:
    sequence: int
    content_type: str
    captured_at: str
    data: bytes
    timestamp_us: int = 0
    width: int = 0
    height: int = 0
    quality: int = 0
    transport: str = "legacy_http"


class MediaBroker:
    """Single-process live media relay with bounded memory."""

    def __init__(self, *, video_target_fps: int = S3_VIDEO_TARGET_FPS) -> None:
        if not S3_VIDEO_MIN_TARGET_FPS <= video_target_fps <= S3_VIDEO_MAX_TARGET_FPS:
            raise ValueError("video_target_fps must be between 8 and 12")
        self._video_target_fps = int(video_target_fps)
        self._video: dict[str, VideoFrame] = {}
        self._video_conditions: dict[str, asyncio.Condition] = {}
        self._video_subscribers: dict[str, set[asyncio.Queue[VideoFrame]]] = {}
        self._s3_video_subscribers: dict[str, set[asyncio.Queue[bytes]]] = {}
        self._video_uploaders: dict[str, set[asyncio.Queue[dict[str, Any]]]] = {}
        self._video_control_sequences: dict[str, int] = {}
        self._video_status: dict[str, dict[str, Any]] = {}
        self._video_errors: dict[str, dict[str, Any]] = {}
        self._video_arrivals: dict[str, deque[float]] = {}
        self._video_subscriber_drops: dict[str, int] = defaultdict(int)
        self._video_connection_totals: dict[str, int] = defaultdict(int)
        self._video_reconnections: dict[str, int] = defaultdict(int)
        self._video_last_close_codes: dict[str, int | None] = {}
        self._video_last_connected_at: dict[str, str] = {}
        self._video_last_closed_at: dict[str, str] = {}
        self._audio_subscribers: dict[str, set[asyncio.Queue[bytes]]] = {}
        self._s3_audio_subscribers: dict[str, set[asyncio.Queue[bytes]]] = {}
        self._audio_uploaders: dict[str, set[asyncio.Queue[dict[str, Any]]]] = {}
        self._audio_control_sequences: dict[str, int] = {}
        self._audio_status: dict[str, dict[str, Any]] = {}
        self._audio_errors: dict[str, dict[str, Any]] = {}
        self._audio_arrivals: dict[str, deque[float]] = {}
        self._audio_subscriber_drops: dict[str, int] = defaultdict(int)
        self._audio_connection_totals: dict[str, int] = defaultdict(int)
        self._audio_reconnections: dict[str, int] = defaultdict(int)
        self._audio_last_close_codes: dict[str, int | None] = {}
        self._audio_last_connected_at: dict[str, str] = {}
        self._audio_last_closed_at: dict[str, str] = {}

    async def publish_video(self, device_id: str, sequence: int, content_type: str, data: bytes) -> None:
        received_at = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        frame = VideoFrame(
            sequence=sequence,
            content_type=content_type,
            captured_at=received_at,
            data=data,
        )
        self._video[device_id] = frame
        self._update_video_status(
            device_id,
            sequence=sequence,
            timestamp_us=0,
            width=0,
            height=0,
            quality=0,
            byte_count=len(data),
            received_at=received_at,
            transport="legacy_http",
        )
        self._video_subscriber_drops[device_id] += self._publish_to_queues(
            self._video_subscribers.get(device_id, set()), frame
        )
        condition = self._video_conditions.setdefault(device_id, asyncio.Condition())
        async with condition:
            condition.notify_all()

    async def publish_s3_video(self, device_id: str, frame: S3VideoFrame) -> None:
        video = VideoFrame(
            sequence=frame.sequence,
            content_type="image/jpeg",
            captured_at=frame.received_at,
            data=frame.payload,
            timestamp_us=frame.timestamp_us,
            width=frame.width,
            height=frame.height,
            quality=frame.quality,
            transport="s3md_wss",
        )
        self._video[device_id] = video
        self._update_video_status(
            device_id,
            sequence=frame.sequence,
            timestamp_us=frame.timestamp_us,
            width=frame.width,
            height=frame.height,
            quality=frame.quality,
            byte_count=len(frame.payload),
            received_at=frame.received_at,
            transport="s3md_wss",
        )
        self._video_subscriber_drops[device_id] += self._publish_to_queues(
            self._video_subscribers.get(device_id, set()), video
        )
        self._video_subscriber_drops[device_id] += self._publish_to_queues(
            self._s3_video_subscribers.get(device_id, set()), frame.wire_data
        )
        condition = self._video_conditions.setdefault(device_id, asyncio.Condition())
        async with condition:
            condition.notify_all()

    def _update_video_status(
        self,
        device_id: str,
        *,
        sequence: int,
        timestamp_us: int,
        width: int,
        height: int,
        quality: int,
        byte_count: int,
        received_at: str,
        transport: str,
    ) -> None:
        self._video_arrivals.setdefault(device_id, deque(maxlen=256)).append(time.monotonic())
        previous = self._video_status.get(device_id, {})
        previous_sequence = previous.get("sequence")
        frame_count = int(previous.get("frame_count", 0)) + 1
        gap_count = int(previous.get("gap_count", 0))
        duplicate_count = int(previous.get("duplicate_count", 0))
        out_of_order_count = int(previous.get("out_of_order_count", 0))
        if previous_sequence is not None and previous.get("transport") == transport:
            delta = (sequence - int(previous_sequence)) & 0xFFFFFFFF
            if delta == 0:
                duplicate_count += 1
            elif 1 < delta < 0x80000000:
                gap_count += delta - 1
            elif delta >= 0x80000000:
                out_of_order_count += 1

        self._video_status[device_id] = {
            "sequence": sequence,
            "timestamp_us": timestamp_us,
            "content_type": "image/jpeg",
            "width": width,
            "height": height,
            "quality": quality,
            "bytes": byte_count,
            "transport": transport,
            "frame_count": frame_count,
            "gap_count": gap_count,
            "duplicate_count": duplicate_count,
            "out_of_order_count": out_of_order_count,
            "received_at": received_at,
        }

    def latest_video(self, device_id: str) -> VideoFrame | None:
        return self._video.get(device_id)

    def record_video_error(self, device_id: str, message: str) -> None:
        previous = self._video_errors.get(device_id, {})
        self._video_errors[device_id] = {
            "invalid_frames": int(previous.get("invalid_frames", 0)) + 1,
            "last_invalid_frame_error": message,
            "last_invalid_frame_at": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        }

    async def wait_video(self, device_id: str, after_sequence: int, timeout: float = 10.0) -> VideoFrame | None:
        frame = self._video.get(device_id)
        if frame and frame.sequence != after_sequence:
            return frame
        condition = self._video_conditions.setdefault(device_id, asyncio.Condition())
        try:
            async with condition:
                await asyncio.wait_for(condition.wait(), timeout=timeout)
        except TimeoutError:
            return None
        frame = self._video.get(device_id)
        return frame if frame and frame.sequence != after_sequence else None

    def _new_video_control(self, device_id: str) -> dict[str, Any]:
        sequence = (self._video_control_sequences.get(device_id, 0) + 1) & 0xFFFFFFFF
        self._video_control_sequences[device_id] = sequence
        subscribers = (
            len(self._video_subscribers.get(device_id, set()))
            + len(self._s3_video_subscribers.get(device_id, set()))
        )
        return {
            "type": "media.control",
            "version": S3_MEDIA_VERSION,
            "sequence": sequence,
            "enabled": subscribers > 0,
            "subscribers": subscribers,
            "target_fps": self._video_target_fps,
        }

    async def _notify_video_uploaders(self, device_id: str) -> None:
        message = self._new_video_control(device_id)
        for queue in tuple(self._video_uploaders.get(device_id, set())):
            if queue.full():
                try:
                    queue.get_nowait()
                except asyncio.QueueEmpty:
                    pass
            try:
                queue.put_nowait(message)
            except asyncio.QueueFull:
                pass

    async def register_video_uploader(self, device_id: str) -> asyncio.Queue[dict[str, Any]]:
        queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue(maxsize=4)
        if self._video_connection_totals[device_id] > 0:
            self._video_reconnections[device_id] += 1
        self._video_connection_totals[device_id] += 1
        self._video_last_connected_at[device_id] = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        self._video_uploaders.setdefault(device_id, set()).add(queue)
        await self._notify_video_uploaders(device_id)
        return queue

    async def next_video_control(
        self,
        device_id: str,
        queue: asyncio.Queue[dict[str, Any]],
    ) -> dict[str, Any]:
        """Return changed control immediately and periodically refresh current state."""
        try:
            return await asyncio.wait_for(
                queue.get(),
                timeout=S3_MEDIA_CONTROL_REFRESH_SECONDS,
            )
        except TimeoutError:
            return self._new_video_control(device_id)

    def unregister_video_uploader(
        self,
        device_id: str,
        queue: asyncio.Queue[dict[str, Any]],
        close_code: int | None = None,
    ) -> None:
        self._video_last_close_codes[device_id] = close_code
        self._video_last_closed_at[device_id] = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        uploaders = self._video_uploaders.get(device_id)
        if not uploaders:
            return
        uploaders.discard(queue)
        if not uploaders:
            self._video_uploaders.pop(device_id, None)

    async def subscribe_video(self, device_id: str) -> AsyncIterator[VideoFrame]:
        queue: asyncio.Queue[VideoFrame] = asyncio.Queue(maxsize=1)
        subscribers = self._video_subscribers.setdefault(device_id, set())
        subscribers.add(queue)
        latest = self._video.get(device_id)
        if latest is not None:
            queue.put_nowait(latest)
        await self._notify_video_uploaders(device_id)
        try:
            while True:
                yield await queue.get()
        finally:
            subscribers.discard(queue)
            if not subscribers:
                self._video_subscribers.pop(device_id, None)
            await self._notify_video_uploaders(device_id)

    async def subscribe_s3_video(self, device_id: str) -> AsyncIterator[bytes]:
        queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=S3_VIDEO_SUBSCRIBER_QUEUE_FRAMES)
        subscribers = self._s3_video_subscribers.setdefault(device_id, set())
        subscribers.add(queue)
        await self._notify_video_uploaders(device_id)
        try:
            while True:
                yield await queue.get()
        finally:
            subscribers.discard(queue)
            if not subscribers:
                self._s3_video_subscribers.pop(device_id, None)
            await self._notify_video_uploaders(device_id)

    async def publish_audio(
        self, device_id: str, sequence: int, sample_rate: int, channels: int, bits: int, data: bytes
    ) -> None:
        self._update_audio_status(
            device_id,
            sequence=sequence,
            timestamp_us=0,
            sample_rate=sample_rate,
            channels=channels,
            bits=bits,
            samples=len(data) * 8 // max(1, channels * bits),
            byte_count=len(data),
            received_at=datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
            transport="legacy_http",
        )
        self._audio_subscriber_drops[device_id] += self._publish_to_queues(
            self._audio_subscribers.get(device_id, set()), data
        )

    async def publish_s3_audio(self, device_id: str, frame: S3AudioFrame) -> None:
        self._update_audio_status(
            device_id,
            sequence=frame.sequence,
            timestamp_us=frame.timestamp_us,
            sample_rate=frame.sample_rate,
            channels=frame.channels,
            bits=frame.bits_per_sample,
            samples=frame.samples_per_frame,
            byte_count=len(frame.payload),
            received_at=frame.received_at,
            transport="s3md_wss",
        )
        self._audio_subscriber_drops[device_id] += self._publish_to_queues(
            self._audio_subscribers.get(device_id, set()), frame.payload
        )
        self._audio_subscriber_drops[device_id] += self._publish_to_queues(
            self._s3_audio_subscribers.get(device_id, set()), frame.wire_data
        )

    def _update_audio_status(
        self,
        device_id: str,
        *,
        sequence: int,
        timestamp_us: int,
        sample_rate: int,
        channels: int,
        bits: int,
        samples: int,
        byte_count: int,
        received_at: str,
        transport: str,
    ) -> None:
        self._audio_arrivals.setdefault(device_id, deque(maxlen=512)).append(time.monotonic())
        previous = self._audio_status.get(device_id, {})
        previous_sequence = previous.get("sequence")
        frame_count = int(previous.get("frame_count", 0)) + 1
        gap_count = int(previous.get("gap_count", 0))
        duplicate_count = int(previous.get("duplicate_count", 0))
        out_of_order_count = int(previous.get("out_of_order_count", 0))
        if previous_sequence is not None and previous.get("transport") == transport:
            delta = (sequence - int(previous_sequence)) & 0xFFFFFFFF
            if delta == 0:
                duplicate_count += 1
            elif 1 < delta < 0x80000000:
                gap_count += delta - 1
            elif delta >= 0x80000000:
                out_of_order_count += 1

        self._audio_status[device_id] = {
            "sequence": sequence,
            "timestamp_us": timestamp_us,
            "sample_rate": sample_rate,
            "channels": channels,
            "bits": bits,
            "samples": samples,
            "bytes": byte_count,
            "transport": transport,
            "frame_count": frame_count,
            "gap_count": gap_count,
            "duplicate_count": duplicate_count,
            "out_of_order_count": out_of_order_count,
            "received_at": received_at,
        }

    def record_audio_error(self, device_id: str, message: str) -> None:
        previous = self._audio_errors.get(device_id, {})
        self._audio_errors[device_id] = {
            "invalid_frames": int(previous.get("invalid_frames", 0)) + 1,
            "last_invalid_frame_error": message,
            "last_invalid_frame_at": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        }

    @staticmethod
    def _publish_to_queues(queues: set[asyncio.Queue[Any]], data: Any) -> int:
        drops = 0
        for queue in tuple(queues):
            if queue.full():
                try:
                    queue.get_nowait()
                    drops += 1
                except asyncio.QueueEmpty:
                    pass
            try:
                queue.put_nowait(data)
            except asyncio.QueueFull:
                drops += 1
        return drops

    def _new_audio_control(self, device_id: str) -> dict[str, Any]:
        sequence = (self._audio_control_sequences.get(device_id, 0) + 1) & 0xFFFFFFFF
        self._audio_control_sequences[device_id] = sequence
        subscribers = len(self._s3_audio_subscribers.get(device_id, set()))
        return {
            "type": "media.control",
            "version": S3_MEDIA_VERSION,
            "sequence": sequence,
            "enabled": subscribers > 0,
            "subscribers": subscribers,
            "target_fps": 0,
        }

    async def _notify_audio_uploaders(self, device_id: str) -> None:
        message = self._new_audio_control(device_id)
        for queue in tuple(self._audio_uploaders.get(device_id, set())):
            if queue.full():
                try:
                    queue.get_nowait()
                except asyncio.QueueEmpty:
                    pass
            try:
                queue.put_nowait(message)
            except asyncio.QueueFull:
                pass

    async def register_audio_uploader(self, device_id: str) -> asyncio.Queue[dict[str, Any]]:
        queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue(maxsize=4)
        if self._audio_connection_totals[device_id] > 0:
            self._audio_reconnections[device_id] += 1
        self._audio_connection_totals[device_id] += 1
        self._audio_last_connected_at[device_id] = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        self._audio_uploaders.setdefault(device_id, set()).add(queue)
        await self._notify_audio_uploaders(device_id)
        return queue

    async def next_audio_control(
        self,
        device_id: str,
        queue: asyncio.Queue[dict[str, Any]],
    ) -> dict[str, Any]:
        """Return changed control immediately and periodically refresh current state."""
        try:
            return await asyncio.wait_for(
                queue.get(),
                timeout=S3_MEDIA_CONTROL_REFRESH_SECONDS,
            )
        except TimeoutError:
            return self._new_audio_control(device_id)

    def unregister_audio_uploader(
        self,
        device_id: str,
        queue: asyncio.Queue[dict[str, Any]],
        close_code: int | None = None,
    ) -> None:
        self._audio_last_close_codes[device_id] = close_code
        self._audio_last_closed_at[device_id] = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        uploaders = self._audio_uploaders.get(device_id)
        if not uploaders:
            return
        uploaders.discard(queue)
        if not uploaders:
            self._audio_uploaders.pop(device_id, None)

    async def subscribe_audio(self, device_id: str) -> AsyncIterator[bytes]:
        queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=4)
        subscribers = self._audio_subscribers.setdefault(device_id, set())
        subscribers.add(queue)
        try:
            while True:
                yield await queue.get()
        finally:
            subscribers.discard(queue)
            if not subscribers:
                self._audio_subscribers.pop(device_id, None)

    async def subscribe_s3_audio(self, device_id: str) -> AsyncIterator[bytes]:
        queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=S3_AUDIO_SUBSCRIBER_QUEUE_FRAMES)
        subscribers = self._s3_audio_subscribers.setdefault(device_id, set())
        subscribers.add(queue)
        await self._notify_audio_uploaders(device_id)
        try:
            while True:
                yield await queue.get()
        finally:
            subscribers.discard(queue)
            if not subscribers:
                self._s3_audio_subscribers.pop(device_id, None)
            await self._notify_audio_uploaders(device_id)

    def status(self, device_id: str) -> dict[str, Any]:
        video = self._video_status.get(device_id)
        audio = self._audio_status.get(device_id)
        video_device_connections = len(self._video_uploaders.get(device_id, set()))
        video_listeners = (
            len(self._video_subscribers.get(device_id, set()))
            + len(self._s3_video_subscribers.get(device_id, set()))
        )
        video_errors = self._video_errors.get(device_id, {})
        audio_errors = self._audio_errors.get(device_id, {})
        now = time.monotonic()
        video_arrivals = [value for value in self._video_arrivals.get(device_id, ()) if now - value <= 5.0]
        audio_arrivals = [value for value in self._audio_arrivals.get(device_id, ()) if now - value <= 5.0]
        video_fps = self._arrival_fps(video_arrivals)
        audio_fps = self._arrival_fps(audio_arrivals)
        video_queue_depth = self._maximum_queue_depth(
            self._video_subscribers.get(device_id, set()),
            self._s3_video_subscribers.get(device_id, set()),
        )
        audio_queue_depth = self._maximum_queue_depth(
            self._audio_subscribers.get(device_id, set()),
            self._s3_audio_subscribers.get(device_id, set()),
        )
        return {
            "video": video,
            "audio": audio,
            "video_listeners": video_listeners,
            "video_wss_listeners": len(self._s3_video_subscribers.get(device_id, set())),
            "video_uploaders": video_device_connections,
            "video_queue_limit": S3_VIDEO_SUBSCRIBER_QUEUE_FRAMES,
            "video_target_fps": self._video_target_fps,
            "video_diagnostics": {
                "video_device_connections": video_device_connections,
                "video_listeners": video_listeners,
                "subscribers": video_listeners,
                "received_frames": int(video.get("frame_count", 0)) if video else 0,
                "received_fps": video_fps,
                "frame_age_ms": self._arrival_age_ms(video_arrivals, now),
                "sequence_gaps": int(video.get("gap_count", 0)) if video else 0,
                "queue_depth": video_queue_depth,
                "queue_limit": S3_VIDEO_SUBSCRIBER_QUEUE_FRAMES,
                "subscriber_drops": self._video_subscriber_drops.get(device_id, 0),
                "reconnects": self._video_reconnections.get(device_id, 0),
                "last_close_code": self._video_last_close_codes.get(device_id),
                "last_connected_at": self._video_last_connected_at.get(device_id),
                "last_closed_at": self._video_last_closed_at.get(device_id),
                "invalid_frames": int(video_errors.get("invalid_frames", 0)),
                "last_invalid_frame_error": video_errors.get("last_invalid_frame_error"),
                "last_invalid_frame_at": video_errors.get("last_invalid_frame_at"),
            },
            "audio_listeners": (
                len(self._audio_subscribers.get(device_id, set()))
                + len(self._s3_audio_subscribers.get(device_id, set()))
            ),
            "audio_wss_listeners": len(self._s3_audio_subscribers.get(device_id, set())),
            "audio_uploaders": len(self._audio_uploaders.get(device_id, set())),
            "audio_queue_limit": S3_AUDIO_SUBSCRIBER_QUEUE_FRAMES,
            "audio_diagnostics": {
                "audio_device_connections": len(self._audio_uploaders.get(device_id, set())),
                "audio_listeners": (
                    len(self._audio_subscribers.get(device_id, set()))
                    + len(self._s3_audio_subscribers.get(device_id, set()))
                ),
                "subscribers": len(self._s3_audio_subscribers.get(device_id, set())),
                "received_frames": int(audio.get("frame_count", 0)) if audio else 0,
                "received_fps": audio_fps,
                "frame_age_ms": self._arrival_age_ms(audio_arrivals, now),
                "sequence_gaps": int(audio.get("gap_count", 0)) if audio else 0,
                "queue_depth": audio_queue_depth,
                "queue_limit": S3_AUDIO_SUBSCRIBER_QUEUE_FRAMES,
                "subscriber_drops": self._audio_subscriber_drops.get(device_id, 0),
                "reconnects": self._audio_reconnections.get(device_id, 0),
                "last_close_code": self._audio_last_close_codes.get(device_id),
                "last_connected_at": self._audio_last_connected_at.get(device_id),
                "last_closed_at": self._audio_last_closed_at.get(device_id),
                "invalid_frames": int(audio_errors.get("invalid_frames", 0)),
                "last_invalid_frame_error": audio_errors.get("last_invalid_frame_error"),
                "last_invalid_frame_at": audio_errors.get("last_invalid_frame_at"),
            },
        }

    @staticmethod
    def _arrival_fps(arrivals: list[float]) -> float:
        if len(arrivals) < 2 or arrivals[-1] <= arrivals[0]:
            return 0.0
        return round((len(arrivals) - 1) / (arrivals[-1] - arrivals[0]), 2)

    @staticmethod
    def _arrival_age_ms(arrivals: list[float], now: float) -> int | None:
        return max(0, round((now - arrivals[-1]) * 1000)) if arrivals else None

    @staticmethod
    def _maximum_queue_depth(*queue_sets: set[asyncio.Queue[Any]]) -> int:
        return max((queue.qsize() for queues in queue_sets for queue in queues), default=0)


def authenticate_device(request: Request, settings: Settings) -> str:
    device_id = request.headers.get("x-device-id", "")
    supplied_key = request.headers.get("x-device-key", "")
    expected_key = settings.device_keys.get(device_id)
    if not expected_key or not hmac.compare_digest(supplied_key, expected_key):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid device credentials")
    return device_id


def decode_i16_le_matrix(data: bytes, rows: int, cols: int) -> list[int]:
    count = rows * cols
    if rows <= 0 or cols <= 0 or rows > 128 or cols > 128:
        raise HTTPException(status_code=422, detail="invalid matrix dimensions")
    if len(data) != count * 2:
        raise HTTPException(status_code=422, detail=f"expected {count * 2} matrix bytes")
    return list(struct.unpack(f"<{count}h", data))


def _event_coalesce_key(message: dict[str, Any]) -> tuple[str, ...] | None:
    """Return the identity of high-rate state that supersedes older state."""
    kind = str(message.get("kind", ""))
    device_id = str(message.get("device_id", ""))
    if kind == "matrix":
        data = message.get("data")
        matrix_kind = str(data.get("kind", "")) if isinstance(data, dict) else ""
        return kind, device_id, matrix_kind
    if kind in {"telemetry", "device", "matrix_status", "media"}:
        return kind, device_id
    return None


def _event_delivery_priority(message: dict[str, Any]) -> int:
    """Keep current sensor state ahead of bursty audit/event traffic."""
    kind = str(message.get("kind", ""))
    if kind == "telemetry":
        return 0
    if kind == "matrix":
        return 1
    if _event_coalesce_key(message) is not None:
        return 2
    return 3


class _EventSubscription:
    """Bounded queue that coalesces replaceable state instead of replaying it."""

    def __init__(self, maxsize: int = 32) -> None:
        self._maxsize = maxsize
        self._items: deque[dict[str, Any]] = deque()
        self._ready = asyncio.Event()

    def put(self, message: dict[str, Any]) -> None:
        key = _event_coalesce_key(message)
        if key is not None:
            self._items = deque(
                item for item in self._items if _event_coalesce_key(item) != key
            )
        if len(self._items) >= self._maxsize:
            # Historical events are durable in SQLite.  Under a model-alert
            # burst, discard the oldest non-state notification before the
            # latest telemetry/matrix state so the dashboard stays live.
            drop_index = next(
                (
                    index
                    for index, item in enumerate(self._items)
                    if _event_coalesce_key(item) is None
                ),
                None,
            )
            if drop_index is None:
                if key is None:
                    return
                drop_index = 0
            del self._items[drop_index]
        self._items.append(message)
        self._ready.set()

    async def get(self) -> dict[str, Any]:
        while not self._items:
            self._ready.clear()
            if self._items:
                break
            await self._ready.wait()
        selected = min(
            range(len(self._items)),
            key=lambda index: (_event_delivery_priority(self._items[index]), index),
        )
        item = self._items[selected]
        del self._items[selected]
        if not self._items:
            self._ready.clear()
        return item


class EventBroker:
    def __init__(self) -> None:
        self._subscribers: set[_EventSubscription] = set()

    async def publish(self, message: dict[str, Any]) -> None:
        for subscription in tuple(self._subscribers):
            subscription.put(message)

    async def subscribe(self) -> AsyncIterator[dict[str, Any]]:
        subscription = _EventSubscription(maxsize=32)
        self._subscribers.add(subscription)
        try:
            while True:
                yield await subscription.get()
        finally:
            self._subscribers.discard(subscription)
