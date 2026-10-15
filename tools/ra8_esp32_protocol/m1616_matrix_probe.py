from __future__ import annotations

import argparse
import time
from dataclasses import dataclass


def require_serial():
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("Missing dependency: pip install pyserial") from exc

    return serial


def crc8_ccitt(data: bytes, init: int = 0xFF) -> int:
    crc = init & 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


@dataclass
class M1616Packet:
    row: int
    col: int
    array_id: int
    packet_id: int
    values: list[int]


class M1616Parser:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.bad_crc = 0
        self.bad_header = 0

    def feed(self, data: bytes) -> list[M1616Packet]:
        self.buffer.extend(data)
        packets: list[M1616Packet] = []

        while True:
            index = self.buffer.find(b"\xFF\xFF")
            if index < 0:
                if len(self.buffer) > 1:
                    del self.buffer[:-1]
                return packets

            if index > 0:
                del self.buffer[:index]
                self.bad_header += index

            if len(self.buffer) < 7:
                return packets

            row = self.buffer[2]
            col = self.buffer[3]
            packet_id = self.buffer[5]
            data_len = self._packet_data_len(row, col, packet_id)
            if data_len is None:
                del self.buffer[0]
                self.bad_header += 1
                continue

            frame_len = 7 + data_len
            if len(self.buffer) < frame_len:
                return packets

            raw = bytes(self.buffer[:frame_len])
            del self.buffer[:frame_len]

            checksum = raw[6]
            calculated = crc8_ccitt(raw[:6] + raw[7:])
            if checksum != calculated:
                self.bad_crc += 1
                continue

            sample_bytes = raw[7:]
            values = [
                int.from_bytes(sample_bytes[i : i + 2], "little", signed=False)
                for i in range(0, len(sample_bytes), 2)
            ]
            packets.append(
                M1616Packet(
                    row=row,
                    col=col,
                    array_id=raw[4],
                    packet_id=packet_id,
                    values=values,
                )
            )

        return packets

    @staticmethod
    def _packet_data_len(row: int, col: int, packet_id: int) -> int | None:
        if row == 16 and col == 16:
            if packet_id in (0, 1):
                return 16 * 7 * 2
            if packet_id == 2:
                return 16 * 2 * 2
        return None


def main() -> None:
    parser = argparse.ArgumentParser(description="Probe an M1616 matrix serial stream")
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--dtr", action="store_true")
    parser.add_argument("--rts", action="store_true")
    args = parser.parse_args()

    serial = require_serial()
    decoder = M1616Parser()
    packet_count = 0
    raw_bytes = 0
    matrix: list[int] = [0] * 256
    packet_ids: set[int] = set()
    array_ids: set[int] = set()

    with serial.Serial(args.port, args.baud, timeout=0.05, rtscts=False, dsrdtr=False) as ser:
        ser.dtr = args.dtr
        ser.rts = args.rts
        ser.reset_input_buffer()
        deadline = time.time() + args.duration
        while time.time() < deadline:
            chunk = ser.read(8192)
            raw_bytes += len(chunk)
            for packet in decoder.feed(chunk):
                packet_count += 1
                packet_ids.add(packet.packet_id)
                array_ids.add(packet.array_id)
                start_row = packet.packet_id * 7
                for i, value in enumerate(packet.values):
                    row = start_row + (i // 16)
                    col = i % 16
                    if row < 16:
                        matrix[row * 16 + col] = value

    nonzero = sum(1 for value in matrix if value)
    print(f"port={args.port} baud={args.baud} raw_bytes={raw_bytes}")
    print(f"packets={packet_count} packet_ids={sorted(packet_ids)} array_ids={sorted(array_ids)[-8:]}")
    print(f"bad_crc={decoder.bad_crc} bad_header={decoder.bad_header} nonzero={nonzero}")
    if packet_count:
        print("matrix:")
        for row in range(16):
            print(" ".join(f"{matrix[row * 16 + col]:5d}" for col in range(16)))


if __name__ == "__main__":
    main()
