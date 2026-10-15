# ESP32-CAM RA8 gateway

This is the canonical ESP32 gateway firmware for the RA8P1 baby monitor. Do
not build copies under temporary extraction directories.

Copy `arduino_config.example.h` to `arduino_config.h`, then set Wi-Fi and
server credentials. The local configuration is ignored by Git.

## Runtime policy

- The current `0.4.9-collection` build deliberately does not initialize the
  ESP32 camera or I2S microphone. It is a dedicated RA8 UART/network gateway.
  RA8 audio collection commands still pass through for SD-based model training.
- RA8 status telemetry is sent every 500 ms with scheduling priority.
- Pressure is latest-only at up to 2 Hz; thermal is latest-only at up to 1 Hz.
- Pressure is recovered from `STATUS_UPLOAD.prs.matrix`; the current RA8 build
  keeps the standalone `0x12` pressure upload disabled.
- Current telemetry plus the latest pressure/thermal matrices and optional
  latest-only remote media share one persistent
  HTTPS connection to `POST /api/v1/batch`. The same `HTTPClient` object remains
  alive between requests so the ESP32 does not repeat the TLS handshake every
  500 ms. Intermediate matrix frames may be dropped when the network is busy.
- Commands, acknowledgements, and reliable safety events use separate HTTPS
  requests so a slow command does not corrupt the batch connection.
- A 30-second network-task heartbeat guard restarts a wedged gateway and reports
  boot count, reset reason, and network-stall restart count after reconnecting.
- A failed batch request resets only its persistent TLS client. The next latest
  batch opens a fresh connection; historical matrices are not replayed.
- UART parsing is time-sliced so sustained RA8 matrix traffic cannot starve the
  control loop. A dedicated higher-priority network watchdog can recover a
  blocked TLS write even while sensor traffic is continuous.
- Wi-Fi reconnects force an immediate status upload, and no more than two
  matrix/control requests may run without another telemetry attempt.
- Safety events remain queued until accepted by the server.
- ESP32 public and LAN media preview are disabled in this build. A
  `media.preview.start` command is acknowledged without enabling media so it
  cannot disturb RA8 telemetry.
- `health.gateway` reports HTTP latency/failures, batch connection opens/reuses/
  resets, UART counters, pending matrices, and successful upload ages.

## Arduino build

Open `esp32cam.ino` from this directory in Arduino IDE.

Use:

- Board package: Espressif ESP32 3.1.1 or newer
- Board: AI Thinker ESP32-CAM
- Partition: Huge APP
- PSRAM: Enabled
- Library: ArduinoJson 7.x
- Library: WebSockets 2.7.2

Media interfaces after Wi-Fi connects:

- `http://<esp32-ip>/` browser player
- `http://<esp32-ip>/stream` MJPEG video
- `ws://<esp32-ip>:82/audio` signed 16-bit little-endian PCM, 16 kHz, mono

The primary cloud status/matrix channel uses persistent HTTPS batches. Legacy
WSS fields remain in telemetry for compatibility and diagnostics.
Audio/video remain real-time LAN streams unless explicitly enabled in the
local configuration.

## Pins

RA8 UART is 921600 8N1:

| ESP32-CAM | RA8P1 |
| --- | --- |
| GPIO1 TX | P602 RX |
| GPIO3 RX | P603 TX |
| GND | GND |

INMP441 remains on GPIO14 BCLK, GPIO15 WS, GPIO13 DATA. GPIO1 and GPIO3 are
also the programming/debug UART. Disconnect RA8 while flashing if upload fails.
After the sketch switches UART0 to 921600, Serial Monitor is no longer a debug
console and any USB-UART traffic can corrupt RA8 communication.
