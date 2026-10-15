# ESP32-P4X-EYE RA8 gateway

This project ports the existing ESP32-S3 gateway protocol core to the
ESP32-P4X-EYE hardware.

Core operation does not require a MicroSD card:

- RA8 UART framing and command forwarding
- telemetry, pressure and thermal forwarding
- ESP32-C6 hosted Wi-Fi
- OV2710 MIPI-CSI video encoded by the ESP32-P4 JPEG hardware
- onboard PDM microphone
- independent telemetry, video and audio network paths

Network paths:

- telemetry and command WSS: `/api/v1/device/stream`, fixed 250 ms cadence
- matrix WSS: `/api/v1/device/matrices`
  - pressure 16x16: up to 8 Hz
  - thermal 80x64: up to 4 Hz
- video WSS: 640x360 JPEG, normally 10 FPS (server-controlled, capped at 12 FPS)
- audio WSS: 16 kHz mono PCM16, 320 samples per frame

Every real-time path has its own connection and bounded/latest-data queue. Slow
video or audio therefore cannot add its upload latency to the telemetry period.
Safety events and command acknowledgements reuse the telemetry WSS, including
event ACK/retry handling. This avoids opening short-lived TLS connections while
four real-time streams are active. HTTPS remains compiled only for legacy
fallback paths; it is not used by normal telemetry, matrices or control flow.

AI story playback is intentionally outside those real-time loops. A
`story.audio.play` command is placed in a one-entry background queue; that task
downloads the authenticated 16 kHz mono PCM16 WAV, verifies its declared size,
SHA-256, CRC32 and WAV format, then prefers a dedicated 1 Mbaud UART using
4096-byte chunks and per-frame acknowledgements. Failed
or interrupted transfers are never reported as complete and cannot be played
as partial files. If the dedicated UART is not wired or does not answer, the
gateway automatically falls back to the existing acknowledged 921600 baud
control UART.

Telemetry and matrix WebSocket buffers hold one complete real-time frame, and
video uses a bounded 32 KiB send window. This prevents each RA8 status or
thermal frame from becoming many small TLS writes without retaining stale
frames or creating unbounded queues.

The P4 build keeps the four TLS/WebSocket paths within the internal-RAM budget
by placing supported Wi-Fi/LwIP allocations in PSRAM while retaining the 64 KiB
TCP windows required by concurrent media traffic. Telemetry reports `free_internal_heap`,
`min_free_internal_heap`, and `largest_internal_block` so long-running tests can
distinguish memory pressure from radio or server faults.

The 20 ms audio WebSocket uses a dedicated TLS transport with `TCP_NODELAY`.
This avoids TCP delayed-ACK/Nagle interactions turning 50 small PCM frames per
second into a bursty stream. WebSocket receive and transmit operations use the
same client lock so one mbedTLS context is never entered concurrently; this is
required by the ESP-Hosted SDIO path after corrupted TLS records were observed
with the separate TX lock. `audio_nodelay_failures` and
`audio_separate_tx_lock` expose both settings.
`event_ws_failures` and `command_ack_ws_failures` expose control-path failures.

The P4 and its onboard ESP32-C6 must run the same ESP-Hosted major/minor
release. The gateway embeds the validated ESP32-C6 SDIO image for 2.12.12 and,
when it detects an older incompatible co-processor or a new embedded image
revision, performs the official ESP-Hosted slave OTA before starting Wi-Fi.
Attempts and the applied image revision are persisted so a same-version
transport fix is installed once without creating a reboot loop. The P4 host and
C6 slave both enable the ESP-Hosted SDIO software checksum; enabling only one
side is invalid. Telemetry exposes both versions, checksum mode, image revision,
OTA result, attempt count and transferred bytes.

Updating a board whose C6 still has checksum disabled is intentionally a
two-stage operation. First flash a P4 build whose generated `sdkconfig` keeps
`ESP_HOSTED_SDIO_CHECKSUM` disabled; this installs the embedded checksum-enabled
C6 image. Then flash the normal build generated from `sdkconfig.defaults`, where
the host checksum is enabled. The staged update is required because host and
slave must use the same checksum mode while the OTA data is transferred.

MicroSD is reserved for future local recording and offline buffering. A missing
card must not stop UART, telemetry or media streaming.

Build credentials are supplied through the environment and are not stored in
the project:

```powershell
$env:GATEWAY_WIFI_SSID = "..."
$env:GATEWAY_WIFI_PASSWORD = "..."
$env:GATEWAY_DEVICE_ID = "..."
$env:GATEWAY_DEVICE_KEY = "..."
idf.py build
```

Do not reuse a generated `sdkconfig` from another ESP-Hosted target. In
particular, an ESP32-H2/SPI configuration boots far enough to flash but then
trips the GPIO interrupt watchdog on P4-EYE hardware. Generate the production
configuration from the tracked C6/SDIO defaults in an isolated build directory:

```powershell
$env:IDF_TARGET = "esp32p4"
$env:SDKCONFIG_DEFAULTS = (Resolve-Path .\sdkconfig.defaults).Path
idf.py -B build_p4c6 -D "SDKCONFIG=$((Resolve-Path .).Path)\build_p4c6\sdkconfig" reconfigure build
```

Before flashing, verify the generated `build_p4c6/config/sdkconfig.h` contains
`CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6`, `CONFIG_ESP32P4_EYE_C6_BOARD`,
`CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE`, and
`CONFIG_ESP_HOSTED_SDIO_CHECKSUM`.

The current port is validated with ESP-IDF 5.5.4 and the Espressif
`esp-dev-kits` source at commit `ce50c11`. The minimal P4-EYE BSP files and
OV2710 configuration used by this build are vendored under
`../vendor_esp_dev_kits`; the full Espressif demo and generated managed
components are not required in this repository.

The matrix protocol writes dimensions as rows then columns. The RA8 thermal
source is 80 pixels wide by 64 pixels high, so the wire header is
`rows=64, cols=80`; the row-major payload is not transposed. The gateway parses
`matrix_ack`, server error and WebSocket close frames so protocol rejections
are visible in the serial log.

RA8 `scd` is forwarded unchanged as `sensors.environment`. RA8 `bme` is
forwarded unchanged as the nested `sensors.environment.bme688` object. Either
sensor may be absent without affecting the remaining telemetry. Microphone,
audio-player and camera diagnostic objects are forwarded as complete objects,
including their SD recording counters.

The ESP32-C6 network coprocessor firmware must match the ESP-Hosted host
component before final media soak testing. A host/coprocessor version mismatch
can cause RPC timeouts while telemetry, matrices, video and audio are active
at the same time.

RA8 UART defaults:

- P4X-EYE GPIO7 (TX) -> RA8 P602 (RX)
- P4X-EYE GPIO8 (RX) <- RA8 P603 (TX)
- GND <-> GND
- 921600 baud, 8N1, 3.3 V logic

Dedicated story audio UART:

- P4X-EYE J22 pin 18 / GPIO50 (TX) -> RA8 P6 pin 4 / P715 (SCI4 RX)
- P4X-EYE J22 pin 17 / GPIO51 (RX) <- RA8 P6 pin 6 / P714 (SCI4 TX)
- GND <-> GND
- 1000000 baud, 8N1, 3.3 V logic
- this link carries story file data only; telemetry and commands stay on GPIO7/GPIO8
- RA8 emits an `SBL2 READY` frame every 2 seconds; P4 proactively performs a
  `PING/ACK` handshake and uses READY as the resynchronization path
- only three consecutive dedicated-link failures permit the compatibility fallback;
  telemetry exposes handshake success/failure and fallback counters

RA8 offline soft-reset request:

- P4X-EYE J22 pin 5 / GPIO10 -> RA8 expansion P7 pin 9 / P712
- GND <-> GND
- optional 10 kOhm pull-up from P712 to RA8 3.3 V
- GPIO10 is open-drain and only pulls low; never connect this signal to 5 V
- a reset pulse is allowed only when both status frames and every other valid
  RA8 UART frame have been absent for 30 seconds
- after 8 seconds without any raw RX byte, the P4 first reinstalls its own UART
  driver and resets the frame parser; RA8 reset is not considered until this
  local recovery has succeeded
- telemetry reports raw RX bytes, raw-byte age, GPIO8 level transitions and
  local UART recovery counts so a loose TX wire can be distinguished from a
  stalled UART driver
- the first reset is blocked during a 30-second boot grace period and later
  pulses are rate-limited to one per 5 minutes
- this firmware-assisted path cannot recover a completely halted CPU0; use a
  hardware watchdog or the solder-only RESET_n network for that failure mode

Telemetry recovery:

- the WebSocket client's built-in reconnect handles short network outages
- a single send failure never restarts the P4 or interrupts matrix/media links
- manual telemetry recovery starts only after 15 seconds of a connected stall
  or 30 seconds continuously disconnected
- failed recovery attempts are rate-limited and keep the remaining services alive
