from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator


MAGIC = b"\xAA\x55"
HEADER_LEN = 8
CRC_LEN = 2

TYPE_HEARTBEAT = 0x01
TYPE_STATUS_UPLOAD = 0x10
TYPE_ALARM_UPLOAD = 0x11
TYPE_PRESSURE_UPLOAD = 0x12
TYPE_SAFETY_EVENT = 0x13
TYPE_CMD = 0x20
TYPE_CMD_RESULT = 0x21
TYPE_SAFETY_ACK = 0x22
TYPE_FILE_BEGIN = 0x30
TYPE_FILE_CHUNK = 0x31
TYPE_FILE_END = 0x32


@dataclass(frozen=True)
class Frame:
    version: int
    msg_type: int
    sequence: int
    payload: bytes


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_frame(msg_type: int, payload: bytes, sequence: int = 0, version: int = 1) -> bytes:
    header = bytearray()
    header.extend(MAGIC)
    header.append(version & 0xFF)
    header.append(msg_type & 0xFF)
    header.extend((sequence & 0xFFFF).to_bytes(2, "big"))
    header.extend(len(payload).to_bytes(2, "big"))
    raw = bytes(header) + payload
    return raw + crc16_ccitt_false(raw).to_bytes(2, "big")


class FrameParser:
    def __init__(self, max_payload: int = 4096) -> None:
        self._buffer = bytearray()
        self.max_payload = max_payload
        self.bad_crc_count = 0
        self.bad_length_count = 0

    def feed(self, data: bytes) -> Iterator[Frame]:
        self._buffer.extend(data)

        while True:
            magic_index = self._buffer.find(MAGIC)
            if magic_index < 0:
                if len(self._buffer) > 1:
                    del self._buffer[:-1]
                return

            if magic_index > 0:
                del self._buffer[:magic_index]

            if len(self._buffer) < HEADER_LEN:
                return

            payload_len = int.from_bytes(self._buffer[6:8], "big")
            if payload_len > self.max_payload:
                self.bad_length_count += 1
                del self._buffer[0]
                continue

            frame_len = HEADER_LEN + payload_len + CRC_LEN
            if len(self._buffer) < frame_len:
                return

            raw = bytes(self._buffer[:frame_len])
            del self._buffer[:frame_len]

            expected_crc = int.from_bytes(raw[-2:], "big")
            actual_crc = crc16_ccitt_false(raw[:-2])
            if expected_crc != actual_crc:
                self.bad_crc_count += 1
                continue

            yield Frame(
                version=raw[2],
                msg_type=raw[3],
                sequence=int.from_bytes(raw[4:6], "big"),
                payload=raw[8:-2],
            )


def decode_thermal_chunk(payload: bytes) -> tuple[int, int, int, int, list[int]]:
    if len(payload) < 12:
        raise ValueError("thermal chunk payload too short")

    thermal_sequence = int.from_bytes(payload[0:4], "big")
    width = int.from_bytes(payload[4:6], "big")
    height = int.from_bytes(payload[6:8], "big")
    offset = int.from_bytes(payload[8:10], "big")
    count = int.from_bytes(payload[10:12], "big")
    sample_bytes = payload[12:]

    if len(sample_bytes) != count * 2:
        raise ValueError("thermal chunk sample length mismatch")

    samples = [
        int.from_bytes(sample_bytes[i : i + 2], "big", signed=True)
        for i in range(0, len(sample_bytes), 2)
    ]

    return thermal_sequence, width, height, offset, samples


def decode_pressure_chunk(payload: bytes) -> tuple[int, int, int, int, list[int]]:
    if len(payload) < 12:
        raise ValueError("pressure chunk payload too short")

    pressure_sequence = int.from_bytes(payload[0:4], "big")
    width = int.from_bytes(payload[4:6], "big")
    height = int.from_bytes(payload[6:8], "big")
    offset = int.from_bytes(payload[8:10], "big")
    count = int.from_bytes(payload[10:12], "big")
    sample_bytes = payload[12:]

    if len(sample_bytes) != count * 2:
        raise ValueError("pressure chunk sample length mismatch")

    samples = [
        int.from_bytes(sample_bytes[i : i + 2], "big", signed=True)
        for i in range(0, len(sample_bytes), 2)
    ]

    return pressure_sequence, width, height, offset, samples
