# RA8 ESP32 Protocol Tools

This folder contains a PC-side parser/logger/visualizer for `ra8_esp32_protocol.pdf`.

The current ESP32 implementation contract is documented in
[`ESP32_ADAPTER_PROTOCOL.md`](ESP32_ADAPTER_PROTOCOL.md).

Board frame format:

```text
AA 55 | VER | TYPE | SEQ_BE16 | LEN_BE16 | PAYLOAD | CRC16_CCITT_FALSE_BE16
```

Implemented message types:

- `0x10 STATUS_UPLOAD`: JSON payload with TMP117 and thermal summary.
- `0x11 ALARM_UPLOAD`: binary thermal matrix chunk.
- `0x12 PRESSURE_UPLOAD`: binary 16x16 pressure matrix chunk.
- `0x13 SAFETY_EVENT`: reliable JSON safety event; the receiver must acknowledge it.
- `0x20 CMD`: P4/PC-to-board command, including exact `silent_reset`.
- `0x21 CMD_RESULT`: acknowledgement sent before an online silent reset.
- `0x22 SAFETY_ACK`: 4-byte big-endian safety event ID.

Use the CH340-connected PC as an ESP32 protocol endpoint:

```powershell
python tools\ra8_esp32_protocol\esp32_simulator.py --list
python tools\ra8_esp32_protocol\esp32_simulator.py --port COM10 --test
```

The simulator prints safety status, sends `safety_test`, and automatically acknowledges events. Use `--no-ack` to verify the two-second retry behavior.

Pressure uses two simultaneous views. `Pressure Absolute` preserves static loads, while `Pressure Micro` subtracts the first 20 frames and expands weak changes with a power-law color scale. Start the monitor without touching the mat when possible. Use `--pressure-baseline-frames 0` to disable subtraction, `--pressure-gamma 0.3` to expand the low end further, or `--pressure-gain` to multiply the micro-pressure values. Raw 16-bit samples are always written unchanged to CSV.
Audio commands use `{"audio":{"cmd":"play","track":1}}`.

Silent reset has an online and an offline path. With UART available, send
`{"cmd":"silent_reset"}` as type `0x20`; RA8 returns type `0x21`, waits
250 ms, then resets without automatic `BOOT.WAV` playback. If UART/RA8 is
wedged, P4 must instead pull the RA8 RESET network at J1/P1 pin 75 or SW203 low
for 150 ms using a 3.3 V open-drain output, then release it to high impedance.
Never drive RESET high. See
[`docs/P4_RA8_SILENT_RESET_PROTOCOL_zh-CN.md`](../../docs/P4_RA8_SILENT_RESET_PROTOCOL_zh-CN.md).

Run:

```powershell
python tools\ra8_esp32_protocol\ra8_live_monitor.py --port COM16 --baud 921600
```

Use `--baud 921600` after changing the FSP UART baudrate to 921600.

Audio control UI:

```powershell
python tools\ra8_esp32_protocol\ra8_audio_control.py --port COM16 --baud 921600
```

LAN serial bridge:

```powershell
python tools\ra8_esp32_protocol\ra8_serial_bridge.py --port COM16 --baud 921600 --host 0.0.0.0 --http-port 8765
```

Only this bridge process opens the local COM port. Other computers on the same LAN can read:

```text
http://<host-ip>:8765/status
http://<host-ip>:8765/pressure
http://<host-ip>:8765/thermal
http://<host-ip>:8765/stream
```

For example:

```powershell
curl http://192.168.1.23:8765/status
curl http://192.168.1.23:8765/pressure
```

The bridge automatically acknowledges valid reliable safety events (`0x13`)
after placing them in its in-memory event history. Bridge status reports
`safety_event_count` and `safety_ack_count` so retry/ACK behavior is visible.

SD card layout for audio:

```text
E:\MUSIC\BOOT.WAV     # played once after boot
E:\MUSIC\*.WAV        # listed in the UI, excluding BOOT.WAV
```

Supported audio buttons/commands: boot, play selected track, pause, resume, stop, previous, next, refresh list, and volume.
The status JSON includes `aud.file`, `aud.elapsed`, `aud.duration`, `aud.vol`, `aud.track`, `aud.count`, and `aud.tracks`.

Microphone status is included in the status JSON as `mic`.
Sampling starts automatically after boot, while SD WAV recording remains off
until `mic_record` is sent.
Useful command payload tokens are:

```text
mic_start
mic_stop
mic_record
mic_record_stop
```

When `mic_record` is active, CPU1 writes 16 kHz mono 16-bit WAV files to `/sd0/MIC/MICxxxxxx.WAV`.
The status JSON fields `mic.peak`, `mic.mean`, `mic.cb`, `mic.read`, `mic.err`, `mic.rec_bytes`, and `mic.rec_sec`
are the first-level bring-up indicators.

Useful runtime options:

```powershell
python tools\ra8_esp32_protocol\ra8_live_monitor.py --port COM16 --baud 921600 --draw-hz 10 --save-every 10
```

- `--draw-hz`: limits GUI redraw rate. A lower value reduces PC load.
- `--save-every`: saves compressed thermal logs every N complete frames. The tool also saves once on exit.
- The thermal window title shows the valid-point percentage. If it is far below 100%, the board is sending invalid thermal samples, not just drawing slowly.

Probe the raw HTPA thermal module over its direct USB/TTL serial port:

```powershell
python tools\ra8_esp32_protocol\thermal_raw_probe.py --list
python tools\ra8_esp32_protocol\thermal_raw_probe.py --port COM16 --scan --duration 2
python tools\ra8_esp32_protocol\thermal_raw_probe.py --port COM16 --baud 460800 --duration 3 --command auto
python tools\ra8_esp32_protocol\thermal_raw_probe.py --port COM16 --baud 115200 --duration 3 --command auto
python tools\ra8_esp32_protocol\thermal_raw_probe.py --port COM16 --baud 921600 --duration 3 --command auto
```

This prints the raw byte count, candidate frame headers, candidate frame lengths, and a few decoded temperature samples. Use it with the thermal module connected directly to the PC first; it is intended to verify the actual module baudrate and frame format before debugging the RA UART path.

Outputs are saved under `tools/ra8_esp32_protocol/logs/` by default:

- `status_YYYYMMDD_HHMMSS.jsonl`
- `tmp117_YYYYMMDD_HHMMSS.csv`
- `thermal_frames_YYYYMMDD_HHMMSS.npz`
