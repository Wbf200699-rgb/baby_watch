# RA8P1 Baby Monitor

多模态预警规则、传感器角色和服务器数据采集步骤见 [docs/MULTIMODAL_WARNING_AND_COLLECTION_GUIDE_zh-CN.md](docs/MULTIMODAL_WARNING_AND_COLLECTION_GUIDE_zh-CN.md)。

This repository contains the RA8P1 dual-core firmware, PC protocol tools, and
the monitoring/training-data server.

## Runtime Ownership

- **CPU0 / Cortex-M85**: microphone capture, future NPU inference, multimodal
  fusion, reliable safety events, ESP32 UART, and the legacy USB camera probe.
- **CPU1 / Cortex-M33**: thermal UART, M1616 pressure UART, TMP117 matrix,
  SCD41/BME688 environment sensing, SD card, system logs, microphone WAV
  writer, and speaker WAV playback.
- **ESP32-CAM**: network gateway and future camera source. It forwards RA8
  summaries and inference results; large training data stays on SD.
- **Server**: device status, alerts, commands, labels, model packages, and
  SD-collection session metadata.

The authoritative RA8-to-ESP32 contract is
[`tools/ra8_esp32_protocol/ESP32_ADAPTER_PROTOCOL.md`](tools/ra8_esp32_protocol/ESP32_ADAPTER_PROTOCOL.md).
The ESP32-to-server contract is
[`server/ESP32_SERVER_PROTOCOL.md`](server/ESP32_SERVER_PROTOCOL.md).
The canonical gateway sketch is
[`firmware/esp32cam/esp32cam.ino`](firmware/esp32cam/esp32cam.ino); do not
flash copies under `temp_32` or extraction directories.

## Shared SRAM

CPU0 and CPU1 exchange fixed-layout mailboxes in shared SRAM. The matching
headers under both projects must remain byte-for-byte identical.

| Region | Address | Owner/payload |
| --- | --- | --- |
| Camera diagnostics | `0x221CFF00` | CPU0 USB camera state |
| Thermal summary | `0x221D0000` | CPU1 thermal status |
| TMP117 summary | `0x221D0100` | CPU1 bed temperatures |
| Environment | `0x221D0180` | CPU1 SCD41 CO₂ plus BME688 temperature, RH, pressure and raw compensated gas resistance |
| Thermal 80x64 points | `0x221D0200` | CPU1 thermal frame |
| Speaker/audio | `0x221D3000` | CPU0 commands, CPU1 status |
| Pressure 16x16 | `0x221D3400` | CPU1 M1616 frame |
| Microphone | `0x221D3800` | CPU0 samples, CPU1 WAV writer |
| SD diagnostics | `0x221D3F00` | CPU1 SDHI/FAT recovery state |

Writers publish payload fields first, execute a memory barrier, and update the
sequence last. Readers that copy matrices verify the sequence before and after
the copy.

## Boot Behavior

- Microphone sampling starts automatically about three seconds after CPU0
  boots. SD recording remains disabled until `mic_record` is received.
- Fusion runs every 100 ms and publishes debounced state `0..11`.
- The reliable `safe` event stream mirrors fusion and retries until ESP32 ACKs.
- Speaker playback and SD logging remain owned by CPU1.
- Placeholder model/cloud threads suspend themselves until their implementation
  is added; they do not wake every RTOS tick.

## Build And Test

Build both e2 studio projects after generating FSP content:

```powershell
cd Multicore_top_CPU0\Debug
make all -j8
cd ..\..\Multicore_top_CPU1\Debug
make all -j8
```

Run host checks:

```powershell
server\.venv\Scripts\python.exe -m unittest discover -s server\tests -v
python -m compileall -q tools server\app server\tools server\tests
node --test server\tests\test_telemetry_history.js
node --check server\static\app.js
```

Build the ESP32 gateway with Arduino IDE (AI Thinker ESP32-CAM, Huge APP,
PSRAM enabled). Copy `firmware/esp32cam/arduino_config.example.h` to the
Git-ignored `arduino_config.h` before building.

Validate a flashed board through CH340:

```powershell
python tools\ra8_esp32_protocol\esp32_simulator.py --list
python tools\ra8_esp32_protocol\esp32_simulator.py --port COM3 --baud 921600
```

Expected healthy values include thermal `s=3`, `valid=5120`, matching CRCs,
TMP117 mask `ok=31`, microphone `s=2` after automatic start, and fusion
`faults=0` when all installed sensors are fresh.

Serial-camera and thermal long-run recovery behavior, wiring, diagnostic
fields, and fault-injection checks are documented in
[`docs/SENSOR_UART_RECOVERY_zh-CN.md`](docs/SENSOR_UART_RECOVERY_zh-CN.md).
SCD41 wiring, IIC1/FSP settings, shared-memory status, and end-to-end telemetry
are documented in
[`docs/SCD41_INTEGRATION_zh-CN.md`](docs/SCD41_INTEGRATION_zh-CN.md).
BME688 shared-IIC1 wiring, raw-gas semantics, P4 field mapping, and J-Link
acceptance are documented in
[`docs/BME688_INTEGRATION_zh-CN.md`](docs/BME688_INTEGRATION_zh-CN.md).
The online UART reset, Dupont-accessible P712 request line, and optional
solder-only RESET_n fallback are documented in
[`docs/P4_RA8_SILENT_RESET_PROTOCOL_zh-CN.md`](docs/P4_RA8_SILENT_RESET_PROTOCOL_zh-CN.md).

## Generated Code

Files under `ra_gen`, `ra_cfg`, and `ra` are generated or vendor-owned. Change
FSP stacks, pins, clocks, interrupts, and thread stack sizes in
`configuration.xml` through e2 studio, then regenerate project content. Keep
application behavior in `src` and shared protocol definitions synchronized
between CPU0 and CPU1.
