# ESP32-S3 gateway protocol mapping

## RA8P1 UART

- Electrical link: 3.3 V UART, 921600 baud, 8 data bits, no parity, 1 stop bit.
- ESP32-S3-CAM RX is GPIO1 and TX is GPIO2.
- The existing RA8 framed UART format, message types, sequence handling and CRC
  are unchanged.
- Wiring is crossed: S3 GPIO2 TX -> RA8 P602 RXD0, S3 GPIO1 RX <- RA8 P603
  TXD0, and both boards must share ground.
- Server commands are translated to current RA8 raw ASCII payloads. They are
  not encoded as JSON on UART:
  - `status`
  - `safety_test`
  - `mic_record` or `mic_record:<filename>`
  - `mic_record_stop`
  - `camera_record` or `camera_record:<filename>`
  - `camera_record_stop`
  - `camera_capture` or `camera_capture:<filename>`
- Optional filenames are validated as 1-31 ASCII characters from
  `A-Z`, `a-z`, `0-9`, `_`, `-`, and `.`.

### Boot readiness handshake

The gateway must initialize UART and start its UART receive/parser task before
Wi-Fi, TLS, camera, microphone, or server initialization.  The UART task sends
the ASCII payload `gateway_ready` as message type `0x20` every 2 seconds for
the lifetime of the process.  It is deliberately periodic rather than a
one-shot notification so resetting either processor recovers the link without
requiring a second manual reset.

The binary frame is:

| Offset | Size | Encoding | Value |
| ---: | ---: | --- | --- |
| 0 | 2 | bytes | `AA 55` |
| 2 | 1 | uint8 | protocol version `1` |
| 3 | 1 | uint8 | message type `0x20` |
| 4 | 2 | big-endian uint16 | transport sequence, increment after each successful send |
| 6 | 2 | big-endian uint16 | payload length, `13` for `gateway_ready` |
| 8 | N | raw bytes | payload, no NUL and no newline |
| 8+N | 2 | big-endian uint16 | CRC-16/CCITT-FALSE over header and payload |

CRC parameters are polynomial `0x1021`, initial value `0xFFFF`, no reflection,
no final XOR.  For transport sequence `1`, the complete reference frame is:

```text
AA 55 01 20 00 01 00 0D 67 61 74 65 77 61 79 5F 72 65 61 64 79 3F 10
```

RA8 continuously parses RX while waiting.  Before the first valid handshake it
suppresses normal status, safety, and matrix uploads so the S3 UART ring cannot
fill during slow Wi-Fi/TLS startup.  For backward compatibility, RA8 sends one
status probe every 5 seconds after an initial 10-second delay.  Once a valid
`gateway_ready` frame is received, RA8 resumes its normal 500 ms status loop.

Acceptance criteria:

1. Power both processors at the same time: telemetry starts without resetting
   RA8 and no later than 2 seconds after the S3 UART task starts.
2. Reset RA8 only: the periodic handshake re-enables uploads within 2 seconds.
3. Reset S3 only: RA8 pauses while S3 boots, then resumes without RX overflow.
4. A bad version, length above 4096, or CRC mismatch must not mark the gateway
   ready.

## Server real-time WSS transport

The gateway opens one persistent authenticated telemetry/command connection to
`WSS /api/v1/device/stream`. The handshake uses `X-Device-ID` and
`X-Device-Key`; credentials never appear in a frame. A clean server close,
transport failure, or Wi-Fi interruption triggers automatic reconnect, and
every new WSS session begins with a fresh `hello`.

Text frames use this envelope:

```json
{"kind":"telemetry","payload":{"sequence":1}}
```

The supported device-to-server kinds are `hello`, `telemetry`, `event`,
`command_ack`, and `heartbeat`. Server replies use `hello_ack`,
`telemetry_ack`, `event_ack`, `command_ack_received`, and `heartbeat_ack`;
commands arrive as `command`.

Telemetry production is anchored to a 500 ms FreeRTOS clock with
`vTaskDelayUntil`, so network upload time is not added to the sampling period.
The producer writes a single latest-value slot. The telemetry WSS sender
copies that slot before sending; matrix traffic never uses this connection.
When the network is congested, an older unsent status is overwritten instead
of building a backlog. HTTPS batch upload is not used by this firmware's
real-time path. At most one telemetry frame may be awaiting
`telemetry_ack`; while it is in flight the producer continues replacing the
latest slot. A 4-second ACK timeout discards the stale in-flight status and
retries only the newest slot on the same healthy WSS. Only three consecutive
ACK timeouts rebuild the gateway WSS, send a new `hello`, and resume with the
newest status; a successful ACK resets the consecutive-timeout counter.
A telemetry write uses a 3-second transport budget so a short
public-network stall does not tear down an otherwise healthy TLS session and
create a much longer reconnect gap. The application ACK deadline starts only
after the synchronous write succeeds. The dedicated TX lock avoids contention
with receive processing, and TCP keepalive detects a half-open connection.
The independent 500 ms producer never waits for that send and never adds the
upload duration to its next period; while a send is slow, the latest slot
overwrites stale status instead of creating a backlog.

Only compact sensor summaries belong in telemetry. In particular, pressure
`matrix`/`raw` samples and thermal image samples are excluded from the JSON and
use the bounded binary matrix path below. The RA8 `mic` object's compact CPU1
SD-card diagnostics (`sdr`, `sds`, `sdfs`, `sdfl`, `sdtry`, `sdfail`, `sdrec`,
`sdfsp`, and `sdff`) remain in `sensors.microphone`; the server uses them to
report SD readiness, initialization progress, retries, and FSP/FAT failures.

Pressure and thermal data use the independent authenticated
`WSS /api/v1/device/matrices` connection. Frames have a fixed 14-byte
network-byte-order header matching `>4sBBHHI`:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `RA8M` |
| 4 | 1 | version `1` |
| 5 | 1 | kind: `1` thermal, `2` pressure |
| 6 | 2 | rows |
| 8 | 2 | columns |
| 10 | 4 | sequence |

The payload is `rows * columns` signed int16 samples in little-endian order.
The strict server schema uses thermal rows=64/cols=80 and pressure
rows=16/cols=16. Pressure targets 4 Hz and full-resolution thermal targets
2 Hz. Both are latest-only: a newer RA8 source frame replaces an unsent older
source frame.

Pressure and thermal maintain independent gateway upload sequences instead of
reusing the faster RA8 source sequences. At most one frame per kind awaits its
matching `matrix_ack`. The exact preallocated binary frame remains retained
until acknowledged; after a 2-second ACK timeout it is retransmitted with the
same sequence. Only a successful ACK clears that source frame and advances the
kind's upload sequence. The matrix WSS has its own client, event handler and
sender task, so a slow matrix write or reconnect cannot block telemetry,
events, commands, audio or video.

Safety events retain their bounded reliable queue and leave it only after the
matching `event_ack`. The S3 validates server commands, forwards the mapped raw
payload to RA8, and returns `command_ack` on this same WSS. The normal 2 Hz
telemetry stream already keeps the WebSocket active, so transport PING is only
a 5-minute fallback and ping/pong timeout disconnect is disabled. Failed
application writes and the bounded telemetry-ACK state machine detect dead
connections. An application heartbeat is sent only if the telemetry producer
itself has been idle for 5 seconds, so an ACK delay cannot compound congestion
with redundant heartbeat writes.

## External S3 microphone

The external INMP441 microphone is deliberately independent of RA8 telemetry
traffic. It uploads PCM16LE through the audio WSS endpoint with the 32-byte
`S3MD` header: 16 kHz, mono, 320 samples/640 bytes per frame. Its bounded
eight-frame queue drops the oldest frame under congestion. Subscriber control,
reconnect and heartbeat remain active on a connection separate from telemetry.
Every new WSS session resets the channel's control-sequence baseline, including
sessions created after a clean server `CLOSE`. The first frame after reconnect,
capture resume, or queue overrun carries the discontinuity flag.

## Onboard camera

The currently fitted OV2640 uses the ESP32-S3-EYE-compatible DVP mapping. It captures
JPEG at VGA 640x480 and targets 8 FPS. The device opens
`/api/v1/media/device/video` as a separate authenticated WSS connection.
Each binary message contains the 32-byte network-byte-order `S3MD` header
followed by one JPEG. For JPEG, `media_type=1`, `meta0=width`,
`meta1=height`, `meta2=quality`, and `meta3=0`.

The server sends versioned `media.control` JSON on the same connection.
Capture pauses when there are no browser subscribers. The device queue has
capacity one and replaces stale frames. Persistent congestion lowers JPEG
quality and then switches to QVGA at 5 FPS; a sustained healthy connection
restores VGA.
The control parser accepts WebSocket text delivered in multiple receive
chunks. `CONNECTED`, `CLOSED`, `DISCONNECTED`, and transport-error events all
reset per-session control state, so a server restart cannot strand video in
the paused state.
