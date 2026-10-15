from __future__ import annotations

import argparse
import json
import threading
import time

from ra8_protocol import (
    FrameParser,
    TYPE_CMD,
    TYPE_SAFETY_ACK,
    TYPE_SAFETY_EVENT,
    TYPE_STATUS_UPLOAD,
    encode_frame,
)


STATE_NAMES = {
    0: "NORMAL",
    1: "NEAR_EDGE",
    2: "OUT_OF_BED",
    3: "OVERHEAT",
    4: "UNDERHEAT",
    5: "SENSOR_FAULT",
    6: "CRYING",
    7: "FACE_COVERED",
    8: "STANDING",
    9: "MOVING",
    10: "SLEEPING",
    11: "AWAKE",
    255: "TEST_ALARM",
}


def require_serial():
    try:
        import serial
        import serial.tools.list_ports
    except ImportError as exc:
        raise SystemExit("Missing dependency: pip install pyserial") from exc
    return serial


def reader(ser, stop: threading.Event, auto_ack: bool) -> None:
    parser = FrameParser()
    last_safety_sequence = None
    while not stop.is_set():
        data = ser.read(4096)
        if not data:
            continue
        for frame in parser.feed(data):
            if frame.msg_type == TYPE_STATUS_UPLOAD:
                try:
                    status = json.loads(frame.payload.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                safe = status.get("safe", {})
                sequence = safe.get("seq")
                if sequence != last_safety_sequence:
                    last_safety_sequence = sequence
                    state = int(safe.get("state", 0))
                    print(
                        "STATUS",
                        STATE_NAMES.get(state, str(state)),
                        f"confidence={safe.get('conf')}",
                        f"sources=0x{int(safe.get('sources', 0)):02x}",
                        f"faults=0x{int(safe.get('faults', 0)):02x}",
                        f"pressure={safe.get('p_total')}/{safe.get('p_edge')}",
                        f"pending={safe.get('pending')}",
                    )
            elif frame.msg_type == TYPE_SAFETY_EVENT:
                try:
                    event = json.loads(frame.payload.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    print("bad safety event:", exc)
                    continue
                event_id = int(event.get("event", 0))
                state = int(event.get("state", 0))
                print("EVENT", event_id, STATE_NAMES.get(state, str(state)), json.dumps(event, separators=(",", ":")))
                if auto_ack and event_id:
                    payload = event_id.to_bytes(4, "big")
                    ser.write(encode_frame(TYPE_SAFETY_ACK, payload, sequence=frame.sequence))
                    print("ACK", event_id)


def main() -> int:
    parser = argparse.ArgumentParser(description="Act as an ESP32 endpoint on the RA8 UART")
    parser.add_argument("--port", help="Serial port, for example COM10")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--list", action="store_true", help="List serial ports")
    parser.add_argument("--no-ack", action="store_true", help="Leave events pending to test retry")
    parser.add_argument("--test", action="store_true", help="Send safety_test after connecting")
    args = parser.parse_args()
    serial = require_serial()

    if args.list:
        for port in serial.tools.list_ports.comports():
            print(port.device, port.description)
        return 0
    if not args.port:
        parser.error("--port is required unless --list is used")

    stop = threading.Event()
    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        worker = threading.Thread(target=reader, args=(ser, stop, not args.no_ack), daemon=True)
        worker.start()
        if args.test:
            ser.write(encode_frame(TYPE_CMD, b"safety_test", sequence=1))
        print("ESP32 simulator connected. Commands: test, status, quit")
        try:
            while True:
                command = input("> ").strip().lower()
                if command in {"quit", "exit", "q"}:
                    break
                if command == "test":
                    ser.write(encode_frame(TYPE_CMD, b"safety_test", sequence=int(time.time()) & 0xFFFF))
                elif command:
                    ser.write(encode_frame(TYPE_CMD, command.encode("utf-8"), sequence=int(time.time()) & 0xFFFF))
        except (KeyboardInterrupt, EOFError):
            pass
        finally:
            stop.set()
            worker.join(timeout=1.0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
