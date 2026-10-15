from __future__ import annotations

import argparse
import time
from collections import Counter

import serial
from serial.tools import list_ports


COMMANDS = {
    "stop": b"\x5A\x01\x00",
    "auto": b"\x5A\x01\x01",
    "once": b"\x5A\x01\x13",
    "query_mode": b"\x5A\x01\x06",
    "save_auto": b"\x5A\x01\x05\x01",
    "save_stop": b"\x5A\x01\x05\x00",
}

DEFAULT_BAUDS = [460800, 115200, 921600]
SCAN_COMMANDS = ["none", "query_mode", "auto", "once"]
FRAME_HEADER = b"\x5A\x08\x46"
FRAME_LENGTH = 10248


def hex_preview(data: bytes, limit: int = 64) -> str:
    return " ".join(f"{byte:02X}" for byte in data[:limit])


def candidate_lengths(buffer: bytes) -> list[tuple[int, int, int, int]]:
    positions = []
    start = 0
    while True:
        pos = buffer.find(b"\x5A", start)
        if pos < 0:
            break
        if pos + 3 < len(buffer):
            positions.append(pos)
        start = pos + 1

    results = []
    for a, b in zip(positions, positions[1:]):
        if b > a:
            results.append((a, b, b - a, buffer[a + 1] if a + 1 < len(buffer) else -1))
    return results


def raw_to_c(raw: int) -> float:
    return (raw - 2732) / 10.0


def analyze_frame(frame: bytes) -> None:
    print(f"frame_len={len(frame)} head={hex_preview(frame, 16)}")
    if len(frame) < 16:
        return

    for offset in (4,):
        values = []
        for i in range(offset, min(len(frame) - 1, offset + 20), 2):
            raw = frame[i] | (frame[i + 1] << 8)
            values.append(f"{raw}:{raw_to_c(raw):.1f}")
        print(f"temps@{offset}: {' '.join(values)}")


def find_complete_frame(buffer: bytearray) -> bytes | None:
    while True:
        start = buffer.find(FRAME_HEADER)
        if start < 0:
            keep = min(len(buffer), len(FRAME_HEADER) - 1)
            if keep:
                del buffer[:-keep]
            else:
                buffer.clear()
            return None

        if start > 0:
            del buffer[:start]

        if len(buffer) < FRAME_LENGTH:
            return None

        frame = bytes(buffer[:FRAME_LENGTH])
        del buffer[:FRAME_LENGTH]
        return frame


def probe_active_trigger(port: str, baud: int, duration: float, trigger_interval: float) -> None:
    print(f"\n=== {port} baud={baud} active_trigger once interval={trigger_interval:.3f}s ===")
    frames = 0
    requests = 0
    rx_bytes = 0
    buffer = bytearray()
    first_frame: bytes | None = None

    with serial.Serial(port, baudrate=baud, bytesize=8, parity="N", stopbits=1, timeout=0.02) as ser:
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        ser.write(COMMANDS["stop"])
        ser.flush()
        print(f"tx={hex_preview(COMMANDS['stop'])}")
        time.sleep(0.2)
        ser.reset_input_buffer()

        end = time.monotonic() + duration
        next_request = time.monotonic()
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= next_request:
                ser.write(COMMANDS["once"])
                ser.flush()
                requests += 1
                next_request = now + trigger_interval

            chunk = ser.read(4096)
            if chunk:
                rx_bytes += len(chunk)
                buffer.extend(chunk)

            while True:
                frame = find_complete_frame(buffer)
                if frame is None:
                    break
                frames += 1
                if first_frame is None:
                    first_frame = frame

    print(f"requests={requests} frames={frames} rx_bytes={rx_bytes} leftover={len(buffer)}")
    if first_frame is not None:
        analyze_frame(first_frame)


def probe_once(port: str, baud: int, duration: float, commands: list[str]) -> None:
    command_label = ",".join(commands) if commands else "none"
    print(f"\n=== {port} baud={baud} command={command_label} ===")
    with serial.Serial(port, baudrate=baud, bytesize=8, parity="N", stopbits=1, timeout=0.05) as ser:
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        for command in commands:
            ser.write(COMMANDS[command])
            ser.flush()
            print(f"tx={hex_preview(COMMANDS[command])}")
            time.sleep(0.2)

        end = time.monotonic() + duration
        data = bytearray()
        while time.monotonic() < end:
            chunk = ser.read(4096)
            if chunk:
                data.extend(chunk)

    print(f"rx_bytes={len(data)}")
    if not data:
        return

    print(f"preview={hex_preview(data, 96)}")
    positions = [pos for pos, byte in enumerate(data) if byte == 0x5A]
    print(f"5A_count={len(positions)} first_positions={positions[:12]}")

    lengths = candidate_lengths(bytes(data))
    if lengths:
        counter = Counter(length for _, _, length, _ in lengths)
        print(f"candidate_frame_lengths={counter.most_common(10)}")
        for start, end, length, second in lengths[:5]:
            print(f"candidate start={start} next={end} len={length} byte1=0x{second:02X}")

        common_len = counter.most_common(1)[0][0]
        for start, end, length, _ in lengths:
            if length == common_len and start + common_len <= len(data):
                analyze_frame(bytes(data[start:start + common_len]))
                break


def list_serial_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return

    for item in ports:
        desc = (item.description or "").encode("ascii", "backslashreplace").decode("ascii")
        hwid = (item.hwid or "").encode("ascii", "backslashreplace").decode("ascii")
        print(f"{item.device}: {desc} [{hwid}]")


def main() -> None:
    parser = argparse.ArgumentParser(description="Probe raw HTPA thermal UART stream.")
    parser.add_argument("--port", help="Serial port, for example COM16")
    parser.add_argument("--baud", type=int, action="append", help="Baudrate to test. Can be repeated.")
    parser.add_argument("--duration", type=float, default=3.0, help="Seconds to capture per baudrate.")
    parser.add_argument("--list", action="store_true", help="List serial ports and exit.")
    parser.add_argument("--scan", action="store_true", help="Try common baudrate/command combinations.")
    parser.add_argument("--active-trigger", action="store_true", help="Send 5A 01 13 periodically and parse returned frames.")
    parser.add_argument("--trigger-interval", type=float, default=0.5, help="Seconds between active trigger commands.")
    parser.add_argument(
        "--command",
        choices=["none", *COMMANDS.keys()],
        action="append",
        help="Command sent before capture. Can be repeated.",
    )
    args = parser.parse_args()

    if args.list:
        list_serial_ports()
        return

    if not args.port:
        raise SystemExit("--port is required unless --list is used")

    bauds = args.baud or DEFAULT_BAUDS
    if args.active_trigger:
        for baud in bauds:
            try:
                probe_active_trigger(args.port, baud, args.duration, args.trigger_interval)
            except serial.SerialException as exc:
                print(f"baud={baud} serial error: {exc}")
        return

    if args.scan:
        for baud in bauds:
            for command_name in SCAN_COMMANDS:
                commands = [] if command_name == "none" else [command_name]
                try:
                    probe_once(args.port, baud, args.duration, commands)
                except serial.SerialException as exc:
                    print(f"baud={baud} command={command_name} serial error: {exc}")
        return

    requested_commands = args.command if args.command is not None else ["auto"]
    commands = [item for item in requested_commands if item != "none"]
    for baud in bauds:
        try:
            probe_once(args.port, baud, args.duration, commands)
        except serial.SerialException as exc:
            print(f"baud={baud} serial error: {exc}")


if __name__ == "__main__":
    main()
