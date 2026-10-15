from __future__ import annotations

import argparse
import time
from pathlib import Path

import serial


def main() -> None:
    parser = argparse.ArgumentParser(description="Timestamped bounded serial capture")
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=180.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    started = time.monotonic()
    deadline = started + args.duration
    args.output.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.2
    port.dtr = False
    port.rts = False
    port.open()
    with port, args.output.open("w", encoding="utf-8", newline="\n") as output:
        while time.monotonic() < deadline:
            raw = port.readline()
            if not raw:
                continue
            elapsed = time.monotonic() - started
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            output.write(f"{elapsed:9.3f} {text}\n")
            output.flush()


if __name__ == "__main__":
    main()
