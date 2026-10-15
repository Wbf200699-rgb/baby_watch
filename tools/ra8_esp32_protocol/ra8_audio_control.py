from __future__ import annotations

import argparse
import json
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk

from ra8_protocol import FrameParser, TYPE_CMD, TYPE_STATUS_UPLOAD, encode_frame


STATUS_TEXT = {
    0: "IDLE",
    1: "WAIT_SD",
    2: "OPEN_ERROR",
    3: "BAD_WAV",
    4: "I2S_ERROR",
    5: "PLAYING",
    6: "DONE",
    7: "PAUSED",
    8: "STOPPED",
}


def require_serial():
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("Missing dependency: pip install pyserial") from exc
    return serial


def fmt_ms(value: int | float | str) -> str:
    try:
        ms = int(value)
    except (TypeError, ValueError):
        return "00:00"
    seconds = max(ms, 0) // 1000
    return f"{seconds // 60:02d}:{seconds % 60:02d}"


class AudioControlApp:
    def __init__(self, root: tk.Tk, port: str, baud: int) -> None:
        self.root = root
        self.root.title("RA8 Audio Control")
        self.serial = require_serial().Serial(port, baud, timeout=0.05)
        self.parser = FrameParser(max_payload=2048)
        self.sequence = 0
        self.queue: queue.Queue[dict] = queue.Queue()
        self.running = True
        self.tracks: list[dict] = []

        self.status_var = tk.StringVar(value="--")
        self.file_var = tk.StringVar(value="--")
        self.time_var = tk.StringVar(value="00:00 / 00:00")
        self.volume_var = tk.IntVar(value=100)
        self.track_var = tk.StringVar()
        self.mic_var = tk.StringVar(value="MIC --")

        self._build_ui()
        self.reader = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader.start()
        self.root.after(100, self._poll_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    def _build_ui(self) -> None:
        frame = ttk.Frame(self.root, padding=10)
        frame.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="Status").grid(row=0, column=0, sticky="w")
        ttk.Label(frame, textvariable=self.status_var).grid(row=0, column=1, sticky="w")
        ttk.Label(frame, text="File").grid(row=1, column=0, sticky="w")
        ttk.Label(frame, textvariable=self.file_var).grid(row=1, column=1, sticky="w")
        ttk.Label(frame, text="Time").grid(row=2, column=0, sticky="w")
        ttk.Label(frame, textvariable=self.time_var).grid(row=2, column=1, sticky="w")

        controls = ttk.Frame(frame)
        controls.grid(row=3, column=0, columnspan=2, pady=(10, 6), sticky="ew")
        for col in range(7):
            controls.columnconfigure(col, weight=1)

        ttk.Button(controls, text="Boot", command=lambda: self._send_cmd("play_boot")).grid(row=0, column=0, padx=2)
        ttk.Button(controls, text="Play", command=self._play_selected).grid(row=0, column=1, padx=2)
        ttk.Button(controls, text="Pause", command=lambda: self._send_cmd("pause")).grid(row=0, column=2, padx=2)
        ttk.Button(controls, text="Resume", command=lambda: self._send_cmd("resume")).grid(row=0, column=3, padx=2)
        ttk.Button(controls, text="Stop", command=lambda: self._send_cmd("stop")).grid(row=0, column=4, padx=2)
        ttk.Button(controls, text="Prev", command=lambda: self._send_cmd("prev")).grid(row=0, column=5, padx=2)
        ttk.Button(controls, text="Next", command=lambda: self._send_cmd("next")).grid(row=0, column=6, padx=2)

        ttk.Label(frame, text="Track").grid(row=4, column=0, sticky="w")
        self.track_combo = ttk.Combobox(frame, textvariable=self.track_var, state="readonly", values=[])
        self.track_combo.grid(row=4, column=1, sticky="ew")

        ttk.Label(frame, text="Volume").grid(row=5, column=0, sticky="w")
        volume = ttk.Scale(frame, from_=0, to=100, variable=self.volume_var, command=self._volume_changed)
        volume.grid(row=5, column=1, sticky="ew")

        ttk.Label(frame, text="Mic").grid(row=6, column=0, sticky="w")
        ttk.Label(frame, textvariable=self.mic_var).grid(row=6, column=1, sticky="w")

        mic_controls = ttk.Frame(frame)
        mic_controls.grid(row=7, column=0, columnspan=2, pady=(8, 0), sticky="ew")
        for col in range(7):
            mic_controls.columnconfigure(col, weight=1)
        ttk.Button(mic_controls, text="Mic Start", command=lambda: self._send_cmd("mic_start")).grid(
            row=0, column=0, padx=2
        )
        ttk.Button(mic_controls, text="Mic Stop", command=lambda: self._send_cmd("mic_stop")).grid(
            row=0, column=1, padx=2
        )
        ttk.Button(mic_controls, text="Rec Start", command=lambda: self._send_cmd("mic_record")).grid(
            row=0, column=2, padx=2
        )
        ttk.Button(mic_controls, text="Rec Stop", command=lambda: self._send_cmd("mic_record_stop")).grid(
            row=0, column=3, padx=2
        )
        ttk.Button(mic_controls, text="Slot Even", command=lambda: self._send_cmd("mic_slot_even")).grid(
            row=0, column=4, padx=2
        )
        ttk.Button(mic_controls, text="Slot Odd", command=lambda: self._send_cmd("mic_slot_odd")).grid(
            row=0, column=5, padx=2
        )
        ttk.Button(mic_controls, text="Slot Auto", command=lambda: self._send_cmd("mic_slot_auto")).grid(
            row=0, column=6, padx=2
        )

        mic_format_controls = ttk.Frame(frame)
        mic_format_controls.grid(row=8, column=0, columnspan=2, pady=(4, 0), sticky="ew")
        for col in range(11):
            mic_format_controls.columnconfigure(col, weight=1)
        ttk.Button(mic_format_controls, text="Shift 16", command=lambda: self._send_cmd("mic_shift16")).grid(
            row=0, column=0, padx=2
        )
        ttk.Button(mic_format_controls, text="Shift 18", command=lambda: self._send_cmd("mic_shift18")).grid(
            row=0, column=1, padx=2
        )
        ttk.Button(mic_format_controls, text="Shift 20", command=lambda: self._send_cmd("mic_shift20")).grid(
            row=0, column=2, padx=2
        )
        ttk.Button(mic_format_controls, text="Shift 22", command=lambda: self._send_cmd("mic_shift22")).grid(
            row=0, column=3, padx=2
        )
        ttk.Button(mic_format_controls, text="Shift 24", command=lambda: self._send_cmd("mic_shift24")).grid(
            row=0, column=4, padx=2
        )
        ttk.Button(mic_format_controls, text="Slew On", command=lambda: self._send_cmd("mic_slew_on")).grid(
            row=0, column=5, padx=2
        )
        ttk.Button(mic_format_controls, text="Slew Off", command=lambda: self._send_cmd("mic_slew_off")).grid(
            row=0, column=6, padx=2
        )
        ttk.Button(mic_format_controls, text="Gain 1x", command=lambda: self._send_cmd("mic_gain1")).grid(
            row=0, column=7, padx=2
        )
        ttk.Button(mic_format_controls, text="Gain 2x", command=lambda: self._send_cmd("mic_gain2")).grid(
            row=0, column=8, padx=2
        )
        ttk.Button(mic_format_controls, text="Gain 4x", command=lambda: self._send_cmd("mic_gain4")).grid(
            row=0, column=9, padx=2
        )
        ttk.Button(mic_format_controls, text="Gain 8x", command=lambda: self._send_cmd("mic_gain8")).grid(
            row=0, column=10, padx=2
        )

        ttk.Button(frame, text="Refresh List", command=lambda: self._send_cmd("refresh")).grid(
            row=9, column=0, columnspan=2, pady=(8, 0), sticky="ew"
        )

    def _reader_loop(self) -> None:
        while self.running:
            data = self.serial.read(4096)
            if not data:
                continue
            for frame in self.parser.feed(data):
                if frame.msg_type == TYPE_STATUS_UPLOAD:
                    try:
                        self.queue.put(json.loads(frame.payload.decode("utf-8")))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        pass

    def _poll_queue(self) -> None:
        while True:
            try:
                status = self.queue.get_nowait()
            except queue.Empty:
                break
            self._update_status(status)
        if self.running:
            self.root.after(100, self._poll_queue)

    def _update_status(self, status: dict) -> None:
        aud = status.get("aud", {})
        state = int(aud.get("s", -1))
        self.status_var.set(STATUS_TEXT.get(state, str(state)))
        self.file_var.set(str(aud.get("file", "")))
        self.time_var.set(f"{fmt_ms(aud.get('elapsed', 0))} / {fmt_ms(aud.get('duration', 0))}")

        volume = int(aud.get("vol", self.volume_var.get()))
        if volume != self.volume_var.get():
            self.volume_var.set(volume)

        tracks = list(aud.get("tracks", []))
        if tracks != self.tracks:
            self.tracks = tracks
            values = [f"{t.get('i', 0)}: {t.get('name', '')} ({fmt_ms(t.get('duration', 0))})" for t in tracks]
            self.track_combo.configure(values=values)
            if values and not self.track_var.get():
                self.track_var.set(values[0])

        mic = status.get("mic", {})
        self.mic_var.set(
            "s={s} q={q} cb={cb} read={read} ev={ev} peak={peak} mean={mean} nz={nz} "
            "rec={rec} slot={slot} shift={shift} slew={slew} gain={gain} wslot={wslot} wepk={wepk} wopk={wopk} "
            "wst={wst} werr={werr} wopen={wopen} wwr={wwr} wbytes={wbytes}".format(
                s=mic.get("s", 0),
                q=mic.get("q", 0),
                cb=mic.get("cb", 0),
                read=mic.get("read", 0),
                ev=mic.get("ev", 0),
                peak=mic.get("peak", 0),
                mean=mic.get("mean", 0),
                nz=mic.get("nz", 0),
                rec=mic.get("rec", 0),
                slot=mic.get("slot", 0),
                shift=mic.get("shift", 0),
                slew=mic.get("slew", 0),
                gain=mic.get("gain", 0),
                wslot=mic.get("wslot", 0),
                wepk=mic.get("wepk", 0),
                wopk=mic.get("wopk", 0),
                wst=mic.get("wst", 0),
                werr=mic.get("werr", 0),
                wopen=mic.get("wopen", 0),
                wwr=mic.get("wwr", 0),
                wbytes=mic.get("wbytes", 0),
            )
        )

    def _send_cmd(self, cmd: str, **kwargs: int) -> None:
        payload = {"audio": {"cmd": cmd}}
        payload["audio"].update(kwargs)
        raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.sequence = (self.sequence + 1) & 0xFFFF
        self.serial.write(encode_frame(TYPE_CMD, raw, self.sequence))

    def _play_selected(self) -> None:
        text = self.track_var.get()
        try:
            track = int(text.split(":", 1)[0])
        except (ValueError, IndexError):
            track = 1
        self._send_cmd("play", track=track)

    def _volume_changed(self, _value: str) -> None:
        now = time.monotonic()
        if not hasattr(self, "_last_volume_send") or (now - self._last_volume_send) > 0.2:
            self._last_volume_send = now
            self._send_cmd("volume", volume=int(self.volume_var.get()))

    def _close(self) -> None:
        self.running = False
        try:
            self.serial.close()
        finally:
            self.root.destroy()


def main() -> int:
    parser = argparse.ArgumentParser(description="RA8 audio protocol test UI")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM16")
    parser.add_argument("--baud", type=int, default=921600, help="UART baudrate")
    args = parser.parse_args()

    root = tk.Tk()
    AudioControlApp(root, args.port, args.baud)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
