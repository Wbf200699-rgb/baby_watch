# RA8P1 to ESP32 UART Protocol

Protocol revision: 5

Revision 5 assigns the Dupont-accessible P4 GPIO10 to RA8 expansion-header
P7-9/P712 as a low-active software-reset request. Revision 4 added the
`silent_reset` command and one-shot `CMD_RESULT`. The binary envelope remains
version `01`.

This document describes the protocol implemented by the current RA8P1 CPU0
firmware. It is intended to be handed directly to the Codex session that
implements the ESP32 gateway.

## 1. Physical interface

| Item | Value |
| --- | --- |
| RA8 peripheral | SCI0 on CPU0 |
| RA8 TX | P603, connect to ESP32 RX |
| RA8 RX | P602, connect to ESP32 TX |
| RA8 soft-reset request | P712, expansion header P7 pin 9 |
| P4 soft-reset output | GPIO10, P4X-EYE J22 pin 5 |
| Electrical level | 3.3 V TTL |
| Baud rate | 921600 |
| Format | 8 data bits, no parity, 1 stop bit |
| Flow control | None on the connector; no RTS/CTS wire is required |

Connect grounds. Do not drive an RA8 UART pin with a 5 V UART signal.

The RA8 UART has a measured configured baud error of about 0.469 percent.

### Dupont-accessible offline reset request

Connect P4X-EYE J22 pin 5 / GPIO10 to RA8 expansion-header P7 pin 9 / P712 and
connect grounds. A 10 kOhm pull-up from P712 to RA8 3.3 V is recommended.
P712 is a firmware-monitored GPIO; it is not the MCU RESET_n signal.

The P4 output must be 3.3 V open-drain:

- Connect P4 and RA8 grounds.
- At P4 boot and normally, leave the GPIO high impedance.
- To request reset, pull P712 low for 150 ms, then release it back to high
  impedance.
- Never actively drive P712 high and never apply 5 V.
- Wait 30 seconds after P4 boot, require 10 seconds of RA8 status loss, and
  enforce a 60-second cooldown between automatic pulses.

RA8 waits five seconds after boot, first observes the released high state, and
requires about 50 ms of continuous low input before calling
`NVIC_SystemReset()`. This path recovers UART/protocol failures while CPU0 and
its scheduler still run. It cannot recover a fully halted CPU0.

For recovery from a complete CPU0/dual-core deadlock, use an independent
hardware watchdog or solder a MOSFET to the true RESET_n network at core-board
J1 pin 75, expansion-board P1 pin 75, or SW203. That optional hardware path is
not available on an ordinary expansion-header pin and is outside the default
Dupont wiring.

## 2. Frame format

Every message in both directions uses the following binary envelope:

```text
Offset  Size  Field
0       2     Magic: AA 55
2       1     Protocol version: 01
3       1     Message type
4       2     Transport sequence, unsigned big-endian
6       2     Payload length N, unsigned big-endian
8       N     Payload
8+N     2     CRC16-CCITT-FALSE, unsigned big-endian
```

Maximum payload length is 4096 bytes. The CRC covers everything from the
first magic byte through the final payload byte. It does not include the CRC
itself.

CRC parameters:

```text
Name:       CRC-16/CCITT-FALSE
Polynomial: 0x1021
Init:       0xFFFF
RefIn:      false
RefOut:     false
XorOut:     0x0000
Wire order: high byte, then low byte
```

The 16-bit transport sequence increments for every transmitted RA8 frame and
wraps naturally at 65535. It is useful for link-loss diagnostics, but matrix
reassembly uses the 32-bit sensor sequence inside the payload.

### Parser requirements

The ESP32 receiver must:

1. Search for `AA 55` and discard preceding bytes.
2. Wait for all 8 header bytes.
3. Reject a payload length greater than 4096 and resume searching for magic.
4. Wait for `8 + N + 2` bytes.
5. Validate the CRC and version before dispatching.
6. On a CRC failure, resume searching for `AA 55`; do not reset the UART.

Use a persistent ring buffer. A single UART read is not guaranteed to contain
one complete frame.

## 3. Message types

| Type | Name | Direction | Current behavior |
| --- | --- | --- | --- |
| `0x01` | HEARTBEAT | RA8 to ESP32 | Reserved, not currently sent |
| `0x10` | STATUS_UPLOAD | RA8 to ESP32 | JSON, sent about every 250 ms |
| `0x11` | THERMAL_CHUNK | RA8 to ESP32 | Binary 80x64 matrix chunks, one complete accepted matrix about every 500 ms |
| `0x12` | PRESSURE_UPLOAD | RA8 to ESP32 | Implemented but disabled by default |
| `0x13` | SAFETY_EVENT | RA8 to ESP32 | Reliable JSON event, requires `0x22` ACK |
| `0x20` | CMD | ESP32 to RA8 | JSON/text command payload |
| `0x21` | CMD_RESULT | RA8 to ESP32 | JSON result sent before an online `silent_reset` |
| `0x22` | SAFETY_ACK | ESP32 to RA8 | Four-byte event ID |
| `0x30` | FILE_BEGIN | Either | Reserved |
| `0x31` | FILE_CHUNK | Either | Reserved |
| `0x32` | FILE_END | Either | Reserved |

The current CPU0 firmware sends `STATUS_UPLOAD` from its 250 ms worker loop.
The status message is also the link heartbeat.

## 4. Status upload (`0x10`)

The payload is UTF-8 JSON without a trailing NUL or newline. Unknown fields
must be ignored so firmware can append diagnostics without breaking ESP32.

Top-level shape:

```json
{
  "tick": 123456,
  "thm": {},
  "tmp": {},
  "scd": {},
  "bme": {},
  "prs": {},
  "mic": {},
  "aud": {},
  "safe": {},
  "fusion": {},
  "vision": {},
  "light": {},
  "pat": {},
  "link": {}
}
```

The sender first builds the full status. If it would exceed the 4096-byte
payload limit it retries with compact detail, then essential detail. The RA8
must never stop status transmission merely because `gateway_ready` has not
arrived.

### `tick`

FreeRTOS tick count on RA8 CPU0. Treat it as an unsigned wrapping counter, not
Unix time.

### `thm`: 80x64 infrared sensor summary

| Field | Meaning |
| --- | --- |
| `seq` | Thermal mailbox sequence |
| `s` | Status: 0 empty, 1 open error, 2 waiting, 3 frame OK, 4 timeout, 5 UART error, 6 CRC error, 7 invalid sensor data |
| `rx` | Received frame bytes; a complete HTPA frame is 10248 |
| `min_cd`, `max_cd`, `ctr_cd`, `avg_cd` | Despite the historical `_cd` name, values are in 0.1 degree C |
| `p01_cd`, `p99_cd`, `mean_cd` | Robust P1/P99 range and mean, also in 0.1 degree C |
| `frame_quality` | 0 normal, 1 transient/NUC candidate held back, 2 invalid |
| `transient_drop_count` | Number of one-frame scene-wide transitions rejected |
| `generation_retry_count` | CPU0 retries caused by concurrent CPU1 matrix publication |
| `last_good_sequence`, `write_generation` | Last accepted frame and seqlock diagnostics |
| `mean_abs_diff_cd`, `changed_pixels` | Difference from the prior accepted frame |
| `valid` | Valid temperature points, normally 5120 |
| `crc_rx`, `crc_calc` | Received and calculated 16-bit sensor-frame CRC values |
| `uart_err`, `pe`, `fe`, `fifo_ovf`, `ovf` | UART aggregate, parity, framing, hardware FIFO overflow, and software-ring overflow counters |
| `crc_bad`, `data_bad` | Cumulative bad CRC and no-valid-point frame counters |
| `w0`, `w1`, `wff` | Number of `0x0000`, `0x0001`, and `0xFFFF` words in the last frame |
| `baud_set` | UART reconfiguration count; normally remains 1 |
| `build` | Thermal driver build marker; current quality-gated build is `0x20260731` (`539363121`) |
| `head`, `raw0`, `raw1`, `rawc`, `ta`, `err` | Low-level diagnostics; do not use as application data |

Application-valid thermal state requires all of the following:

```text
thm.s == 3
thm.rx == 10248
thm.valid == 5120
thm.crc_rx == thm.crc_calc
```

### `tmp`: bed temperature sensors

| Field | Meaning |
| --- | --- |
| `seq` | Sample sequence |
| `s` | Driver status |
| `ok` | Connected-sensor bit mask |
| `err` | Sensor error bit mask |
| `fs`, `ch`, `fsp` | Failure step, channel, and FSP error diagnostics |
| `pca` | Upstream PCA9548A address |
| `t_mc` | Five signed temperatures in 0.001 degree C |
| `addr` | Five I2C addresses |

For sensor index `i`, use it only when `(tmp.ok & (1 << i)) != 0`.

### `scd`: SCD41 environment sensor

| Field | Meaning |
| --- | --- |
| `seq` | Shared-mailbox publication sequence |
| `s` | Status: 0 empty, 1 starting, 2 measuring, 3 sample OK, 4 not found, 5 CRC error, 6 I2C error, 7 timeout, 8 recovering |
| `co2` | CO₂ concentration in ppm |
| `t_mc` | Ambient temperature in 0.001 degree C |
| `rh_mp` | Relative humidity in 0.001 percent RH |
| `ready` | Last data-ready result |
| `crc_err`, `i2c_err`, `to`, `rec` | Cumulative CRC, I2C, timeout, and recovery counters |
| `fsp` | Last FSP error code |
| `last` | CPU0 RA8 tick when the last complete valid sample was observed; directly comparable with top-level `tick` |

The sensor updates about every 5 seconds. The ESP32 gateway maps this complete
object to `sensors.environment` without changing units. A missing `scd` object
means an older RA8 firmware and must not break status forwarding. Ambient
temperature is not the TMP117 bed temperature, and no CO₂ alarm threshold is
defined by this transport contract.

### `bme`: BME688 environment extension

`bme` is a top-level sibling of `scd` and is present in every RA8 status detail
mode. The P4 gateway should place it at `sensors.environment.bme688` without
changing field names or units.

| Field | Meaning |
| --- | --- |
| `s` | Status: 0 empty, 1 starting, 2 measuring, 3 sample OK, 4 not found, 5 driver/data error, 6 I2C error, 7 timeout, 8 recovering |
| `addr` | Detected 7-bit I2C address, `0x76` or `0x77`; zero before detection |
| `t_mc` | Compensated ambient temperature in 0.001 degree C |
| `rh_mp` | Compensated relative humidity in 0.001 percent RH |
| `p_pa` | Compensated atmospheric pressure in Pa |
| `gas_ohm` | Bosch SensorAPI compensated gas-sensor resistance in ohms |
| `flags` | bit 0 new data, bit 1 gas result valid, bit 2 heater stable |
| `last` | CPU0 RA8 tick when the last complete valid sample was observed; directly comparable with top-level `tick` |
| `err` | Cumulative BME688 initialization, I2C, and driver/data error count |

`gas_ohm` is raw compensated resistance, not VOC, VSC, eCO₂, or IAQ. This
firmware does not run Bosch BSEC. Consumers may display gas resistance as
current only when both `flags.bit1` and `flags.bit2` are set. `flags.bit0` may
clear between forced-mode frames while `s=3` and `last` remain fresh; that is
not a sensor fault.

### `prs`: pressure matrix

| Field | Meaning |
| --- | --- |
| `seq` | Pressure frame sequence |
| `s` | Status: 0 empty, 1 open error, 2 OK, 3 partial, 4 error |
| `ok`, `err` | Connected and error masks |
| `fs`, `ch`, `fsp`, `up_ch` | Driver diagnostics |
| `raw` | Up to 32 signed raw ADC values |
| `matrix` | 256 signed values, row-major 16x16 |

The pressure values are raw sensor values. There is no calibrated conversion
to newtons or kilograms in the current firmware. Preserve signed 16-bit
precision when forwarding them.

The full 256-point `prs.matrix` is present in full and compact status detail.
It is an empty array in essential detail so the status heartbeat always fits.
The ESP32 should preserve the last complete matrix and must not treat an empty
essential-detail array as a new zero-pressure frame. Standalone `0x12`
transmission remains disabled by default.

### `mic`: microphone and SD recording

Important fields:

| Field | Meaning |
| --- | --- |
| `seq` | Capture data sequence |
| `cmd_seq`, `ack` | Command sequence and command acknowledgement sequence |
| `s` | 0 idle, 1 open error, 2 running, 3 recording, 4 stopped, 5 I2S error |
| `rate`, `bits` | Sample rate and captured I2S word size |
| `cb`, `q`, `read` | Callback, queued-block, and read counters |
| `err`, `to`, `fsp`, `ev` | Error, timeout, FSP error, and last event |
| `peak`, `mean`, `last`, `nz` | Signal diagnostics |
| `rec`, `rec_bytes`, `rec_sec` | Recording state and progress |
| `wst`, `werr`, `wopen`, `wwr`, `wbytes` | SD WAV writer diagnostics |
| `wgap`, `wclick` | Lost IPC chunks (poll gaps or queue overflow) and rejected impulsive samples |
| `slot`, `shift`, `slew`, `gain`, `wslot` | Recording conversion settings |
| `wepk`, `wopk` | Even and odd I2S slot peak diagnostics |

### `aud`: speaker playback

| Field | Meaning |
| --- | --- |
| `seq` | Playback status sequence |
| `cmd_seq`, `ack` | Command sequence and acknowledgement sequence |
| `s` | 0 idle, 1 wait SD, 2 open error, 3 bad WAV, 4 I2S error, 5 playing, 6 done, 7 paused, 8 stopped |
| `track`, `count` | Current track index and number of tracks |
| `vol` | Volume percent |
| `elapsed`, `duration` | Milliseconds |
| `rate` | WAV sample rate |
| `file` | Current file name |
| `tracks` | Up to eight objects with `i`, `name`, and `duration` |

### `safe`: RA8 hardware safety fusion

| Field | Meaning |
| --- | --- |
| `seq` | Safety-state sequence |
| `state` | 0 normal, 1 near edge, 2 out of bed, 3 overheat, 4 underheat, 5 sensor fault, 6 crying, 7 face covered, 8 standing, 9 moving, 10 sleeping, 11 awake, 255 test alarm |
| `conf` | Confidence, nominal range 0..100 |
| `severity` | 0 informational through 5 emergency |
| `sources` | Source bit mask |
| `since` | RA8 tick when current state began |
| `event` | Most recently allocated safety event ID |
| `pending` | Event ID awaiting ESP32 ACK, zero if none |
| `acks`, `retries` | Reliability counters |
| `faults` | Sensor-fault bit mask |
| `level` | Global alarm level: 0 normal, 1 observe, 2 warning, 3 emergency |
| `incident`, `type`, `action` | Pending reliable incident ID, type, and lifecycle action |
| `p_total`, `p_edge`, `px100`, `py100` | Pressure fusion diagnostics |

Source bits:

```text
bit 0: microphone
bit 1: infrared thermal matrix
bit 2: bed temperature sensors
bit 3: pressure matrix
bit 4: camera/model result
bit 5: SCD41 environment sensor
bit 7: system/test
```

Sensor-fault bits map to thermal, temperature, pressure, microphone, and the
RA8 serial camera/model pipeline in bits 0 through 4 respectively. Bit 5 maps
to the SCD41 environment sensor.

`safe` is the reliable-event projection of the debounced `fusion` result. Its
state, confidence, severity, sources, faults, and pressure diagnostics should
match `fusion`, with a delay of at most one UART status period.

## 5. Thermal matrix chunk (`0x11`)

This historical message type is named `ALARM_UPLOAD` in the RA8 C enum, but its
actual payload is an infrared matrix chunk.

```text
Offset  Size       Field
0       4          Thermal sequence, unsigned big-endian
4       2          Width, always 80
6       2          Height, always 64
8       2          Point offset, unsigned big-endian
10      2          Point count, unsigned big-endian
12      count*2    Signed int16 samples, big-endian
```

Samples are row-major and are in 0.1 degree C. Invalid points are `-32768`.

RA8 sends at most 250 points per chunk. One complete frame therefore consists
of 21 chunks: twenty chunks of 250 points and one final chunk of 120 points.

ESP32 reassembly rules:

1. Key the working buffer by the 32-bit thermal sequence.
2. Allocate 5120 signed 16-bit samples plus a received-range bitmap.
3. Validate width, height, offset, count, and payload length.
4. Copy samples into `[offset, offset + count)`.
5. Publish only after all 5120 positions for the same sequence are present.
6. Drop an incomplete buffer when a newer thermal sequence arrives or after 3 seconds.

No chunk ACK is required. Missing chunks are replaced by the next complete
thermal frame, not retransmitted.

## 6. Pressure matrix (`0x12`)

The payload uses the same chunk header as thermal:

```text
u32_be pressure_sequence
u16_be width = 16
u16_be height = 16
u16_be offset = 0
u16_be count = 256
int16_be samples[256]
```

The current firmware sets `ESP32_UART_UPLOAD_PRESSURE_MATRIX` to zero, so this
message is not sent unless that build option is enabled. Parse it for forward
compatibility, but use `STATUS_UPLOAD.prs.matrix` now.

## 7. Reliable safety event (`0x13` and `0x22`)

`0x13` payload is JSON:

```json
{
  "version": 2,
  "event": 17,
  "tick": 123456,
  "state": 1,
  "incident": 9,
  "type": 7,
  "action": 1,
  "lifecycle": 2,
  "level": 2,
  "global_level": 2,
  "confidence": 82,
  "severity": 3,
  "sources": 24,
  "duration_ms": 500,
  "faults": 0,
  "pressure_total": 12000,
  "pressure_edge": 9000
}
```

`action` is `1=open`, `2=update`, `3=escalate`, `4=resolve`. `lifecycle` is
`0=clear`, `1=candidate`, `2=active`, `3=escalated`, `4=recovery_hold`,
`5=resolved`. Event IDs identify reliable UART deliveries; incident IDs tie
all lifecycle events for one risk episode together.

ESP32 must immediately return message type `0x22`. Its payload is exactly the
four-byte unsigned big-endian `event` value. The outer transport sequence is
independent and may use any local ESP32 transmit counter.

RA8 retries an unacknowledged event about every 2 seconds. ESP32 must make ACK
processing idempotent and ACK duplicate event IDs. Forwarding to the server
and acknowledging RA8 are separate operations: persist or enqueue the event
locally, then ACK RA8 so a temporary network outage does not congest UART.

## 8. Commands from ESP32 (`0x20`)

The payload should be UTF-8 JSON. The current RA8 command handler searches for
tokens rather than using a strict JSON parser, so use the canonical payloads
below and send only one command per frame.

### Speaker

```json
{"cmd":"play_boot"}
{"cmd":"play","track":1}
{"cmd":"pause"}
{"cmd":"resume"}
{"cmd":"stop"}
{"cmd":"next"}
{"cmd":"prev"}
{"cmd":"volume","volume":50}
{"cmd":"refresh"}
```

Track indexes are one-based. Confirm execution by observing `aud.cmd_seq` and
`aud.ack` in subsequent status messages. Legacy audio commands do not emit a
`0x21 CMD_RESULT`.

### Warm light relay

Send either exact ASCII payload in a type-`0x20` frame:

```text
light_on
light_off
```

The equivalent exact JSON forms `{"cmd":"light_on"}` and
`{"cmd":"light_off"}` are also accepted. Do not use a toggle command because
transport retries must be idempotent. Confirm execution from the next RA8
status object `light`: `on` is the commanded output, `s=1` means the P711 GPIO
is ready, `pin=711` identifies the physical output, and `ah=1` means the relay
input is active high. P711 is expansion header P7 pin 12 and boots inactive.

### Silent reset

When UART is healthy, send one of these exact type-`0x20` payloads:

```text
silent_reset
```

```json
{"cmd":"silent_reset"}
```

Unlike legacy token commands, reset matching is exact so an unrelated JSON
string cannot accidentally reboot the RA8. RA8 first transmits type `0x21`
with a JSON payload and waits for UART TX completion:

```json
{
  "cmd": "silent_reset",
  "ok": true,
  "request_sequence": 4660,
  "reset_delay_ms": 250
}
```

`request_sequence` is the incoming `0x20` frame's 16-bit transport sequence,
not the outgoing result frame's sequence. P4 should correlate on that field,
then expect UART/status loss while RA8 resets. RA8 issues `SYSRESETREQ` 250 ms
after the send attempt; it still resets if the result cannot be delivered.
P4 must not resend during the reset and should enforce at least a 5-second
cooldown for online reset commands.

If RA8 is offline and cannot parse the command, pulse the P712 software-reset
request as specified in section 1. No UART acknowledgement is possible for
that path. A completely halted CPU0 still requires a hardware watchdog or the
optional soldered RESET_n path.

CPU1 captures the Renesas CWSF flag in `BSP_WARM_START_POST_C`, after
`SystemRuntimeInit` has initialized ordinary `.bss` but before any RTOS task
starts, and is the only core that writes CWSF. Therefore:

- A true cold power-on (`CWSF=0`) still auto-plays `/sd0/MUSIC/BOOT.WAV`.
- `RESET_n`, `SYSRESETREQ`, watchdog, manual reset-button, and debugger warm
  resets (`CWSF=1`) skip only the automatic boot sound.
- The explicit `play_boot` command remains available after startup.

After programming, power-cycle the board before testing the cold-start sound;
a debugger reset without removing power is intentionally classified as warm.
The boot state is exposed in the audio IPC mailbox at `0x221D3184`; no custom
pre-C SRAM snapshot is used because POR SRAM ECC contents are undefined.

### Microphone

CPU0 starts microphone sampling automatically about three seconds after boot;
this does not start SD recording. `mic_start` remains idempotent and is useful
for recovery after an explicit `mic_stop`.

```json
{"cmd":"mic_start"}
{"cmd":"mic_stop"}
{"cmd":"mic_record"}
{"cmd":"mic_record_stop"}
{"cmd":"mic_slot_even"}
{"cmd":"mic_slot_odd"}
{"cmd":"mic_slot_auto"}
{"cmd":"mic_shift16"}
{"cmd":"mic_shift18"}
{"cmd":"mic_shift20"}
{"cmd":"mic_shift22"}
{"cmd":"mic_shift24"}
{"cmd":"mic_slew_on"}
{"cmd":"mic_slew_off"}
{"cmd":"mic_gain1"}
{"cmd":"mic_gain2"}
{"cmd":"mic_gain4"}
{"cmd":"mic_gain8"}
```

Confirm execution using `mic.cmd_seq`, `mic.ack`, and `mic.s`.

The recording defaults are the even SSI slot, sample shift 8, fixed gain 4x,
and slew limiting effectively disabled (`32767`). The WAV writer keeps its
high-pass and low-pass filters continuous across chunks and does not apply
independent 12 ms RMS normalization, because chunk-local gain changes create
audible 83.33 Hz pumping. CPU0 immediately re-arms SSI/DTC reception into a
16-buffer ring from the RX-complete callback, so cry preprocessing and IPC
publication cannot create a gap between 12 ms capture blocks. Before gain and
the continuous high-pass/low-pass filters, CPU1 applies a five-sample
MAD/Hampel de-clicker to isolated one- or two-sample raw impulses; this targets
digital bit glitches without using a speech-damaging noise gate or independent
chunk gain. Shift commands preserve the current slew setting.

### `fusion`: CPU0 multimodal state

`fusion` is the upper-layer result produced every 100 ms. ESP32 should forward
this object as the server telemetry `inference` source instead of trying to
reimplement the safety rules.

| Field | Meaning |
| --- | --- |
| `seq` | Fusion update sequence |
| `state` | Debounced final state |
| `candidate` | Current pre-debounce candidate state |
| `conf` | Confidence of the final state, 0-100 |
| `severity` | Final-state severity, 0-5 |
| `sources` | Source bit mask: mic, thermal, TMP117, pressure, camera, SCD41 in bits 0-5 |
| `valid` | Currently fresh input bit mask using the same source bits |
| `faults` | Thermal, TMP117, pressure, mic, camera/model and SCD41 fault bits 0-5 |
| `since` | RTOS tick when the final state began |
| `occupied` | Pressure-derived bed occupancy |
| `p_total`, `p_edge`, `px100`, `py100` | Pressure load, edge load and center of pressure |
| `thm_min_cd`, `thm_max_cd` | Legacy field names; infrared values are in 0.1 degree C |
| `bed_min_mc`, `bed_max_mc` | Bed sensor range in 0.001 degree C |
| `cry`, `sleep`, `covered`, `standing`, `movement` | Latest fresh NPU confidence values |
| `sv`, `ev` | Fusion schema version and evidence sequence |
| `ctx` | Eleven parallel context enums in the fixed order below |
| `alarm` | Global level, primary incident, lifecycle, count and type mask |
| `pv` | Pressure edge ratio, activity and X/Y movement evidence |
| `ex` | Explainable fusion evidence; schema v6 appends dataset-calibration diagnostics |
| `comfort` | Schema-v7 comfort action recommendation, reason, confidence, confirmation and block masks |
| `vv` | Vision sequence, age, infant/head presence, edge flag and edge distance |
| `drop` | Fusion reliable-event queue overflow counter |
| `ai` | Active incident summaries; omitted only in essential-detail fallback |

`ctx` is a compact array in this exact order:

```text
[presence, arousal, bed_motion, airway_view, edge_state, distress,
 cry_cause_hint, body_temperature, bed_thermal, caregiver, system_health]
```

Context values:

```text
presence:         0 unknown, 1 present, 2 absent, 3 uncertain
arousal:          0 unknown, 1 sleeping, 2 awake, 3 transition
bed_motion:       0 unknown, 1 quiet, 2 rolling, 3 active
airway_view:      0 unknown, 1 clear, 2 partial, 3 covered, 4 not_visible
edge_state:       0 unknown, 1 center, 2 near, 3 grab, 4 outside
distress:         0 unknown, 1 none, 2 crying, 3 persistent_cry
cry_cause_hint:   0 unknown, 1 hungry, 2 belly_pain, 3 discomfort,
                  4 tired, 5 burping
temperature:      0 unknown, 1 normal, 2 high, 3 low
caregiver:        0 unknown, 1 absent, 2 present
system_health:    0 ok, 1 degraded, 2 blind
```

`alarm` is:

```text
[global_level, primary_type, primary_incident_id, primary_lifecycle,
 active_incident_count, active_type_mask]
```

`ai` contains up to eight arrays:

```text
[type, lifecycle, level, severity, confidence, source_mask,
 incident_id, since_tick]
```

Incident types are stable protocol values:

```text
1 presence_conflict       2 sensor_degraded      3 monitor_blind
4 airway_not_visible      5 airway_partial       6 airway_covered
7 edge_near               8 edge_climbing        9 out_of_bed
10 cry                    11 persistent_cry      12 overheat
13 underheat              14 camera_frozen
```

`pv` is `[edge_ratio_x1000, activity_x1000, move_x100, move_y100]`.
`vv` is `[frame_sequence, age_ms, infant_present, head_present,
infant_at_edge, edge_distance_x1000]`.

`ex` keeps the first 17 schema-v5 entries unchanged and appends four entries
in schema v6:

```text
[presence_score_x1000, positive_source_count,
 pressure_presence_x1000, vision_presence_x1000, thermal_presence_x1000,
 activity_x1000, airway_risk_x1000, edge_risk_x1000,
 thermal_hotspot_points, thermal_hotspot_x1000, thermal_hotspot_y1000,
 thermal_hotspot_mean_deci_c,
 bed_left_milli_c, bed_center_milli_c, bed_right_milli_c,
 bed_spread_milli_c, bed_hotspot_region,
 thermal_bed_x1000, thermal_edge_x1000, position_agreement_x1000,
 calibration_profile_version]
```

The current profile version is `0x20260803` and is generated from the capture
archive identified by `fusion_calibration_generated.h`. Older consumers may
continue reading only the first 17 entries.

Schema v7 adds an advisory-only comfort decision:

```text
comfort = [action_mask, reason_mask, confidence,
           requires_user_confirmation, block_mask]
```

`action_mask` bits are: bit 0 light off, bit 1 light on, bit 2 press the
soother once, bit 3 caregiver check. `reason_mask` bits are: sleeping, cry,
persistent cry, active safety event, monitoring blind spot and baby absent in
bits 0 through 5. `block_mask` bits explain why a pat is not recommended:
danger, no reliable presence evidence and monitoring blind spot in bits 0
through 2.

This object is a model recommendation, not an actuator command. The gateway
must forward it unchanged. It must never turn P711 or P914 solely because an
action bit is set. A user client sends the explicit `light.set` or
`soother.set` command after confirmation, then verifies the independent
actuator telemetry.

### `pat`: soothing motor relay

P914 (expansion header P7 pin 2) is a high-active maintained relay output. It is
inactive at boot. `on` is the GPIO state accepted by RA8, `cmd_seq` counts state
commands, and `err`/`fsp` report GPIO failures. There is no appliance-side
physical feedback.

The gateway maps server command `soother.set` with boolean argument `on` to the
exact RA8 text payload `soother_on` or `soother_off`. Repeating either target
state is safe and must leave the relay in that state.

### `link`: RA8-to-ESP32 UART diagnostics

| Field | Meaning |
| --- | --- |
| `rx` | Bytes received by RA8 from ESP32 |
| `tx` | Completed RA8 UART transmissions |
| `err` | UART parity/framing/overflow event count |
| `ovf` | RA8 software RX ring overflow count |
| `event` | Last FSP UART event value |
| `ready` | 1 after RA8 receives at least one `gateway_ready` command in this boot |
| `ready_n`, `ready_age` | Received handshake count and age in RTOS ticks |
| `st_try`, `st_ok` | Status send attempts and successful UART submissions |
| `st_len`, `st_detail` | Last status payload length and selected detail: 0 full, 1 compact, 2 essential, 3 emergency |
| `st_long`, `st_uart` | Payload-too-long and UART-send failure counters |
| `fw`, `vision_fw`, `rules` | CPU0 firmware, vision runtime, and fusion-rule build markers |
| `cry_fw`, `cry_type_fw` | SHA-256 prefixes for the binary cry and cry-type models |
| `heap`, `heap_min` | Current and minimum-ever CPU0 FreeRTOS heap bytes |
| `stk` | Stack high-water marks in words: camera, vision preprocess, vision inference, audio inference, fusion, ESP32 UART |

The ESP32 sends the plain UTF-8 payload `gateway_ready` as type `0x20` every
two seconds. This checks the S3-TX-to-RA8-RX direction only. It is not a
permission to transmit: RA8 status continues every 500 ms before, during, and
after the handshake. A gateway may use the following reference frame for
transport sequence 1:

```text
AA 55 01 20 00 01 00 0D
67 61 74 65 77 61 79 5F 72 65 61 64 79
3F 10
```

Direction diagnosis:

```text
S3 ready_tx_count grows, uart_rx_bytes stays 0:
    inspect RA8 P603 TX -> S3 RX and the RA8 status sender.
S3 uart_rx_bytes grows, uart_valid_frames stays 0:
    inspect baud, framing, CRC, or mixed UART traffic.
S3 valid frames grow, uart_status_frames stays 0:
    inspect RA8 message type and status payload construction.
RA8 link.ready stays 0 while S3 ready_tx_count grows:
    inspect S3 TX -> RA8 P602 RX.
RA8 link.st_long grows:
    status JSON still exceeds 4096 bytes in essential detail.
```

Fusion state values:

```text
0 normal        1 near edge     2 out of bed
3 overheat      4 underheat     5 sensor fault
6 crying        7 face covered  8 standing
9 moving       10 sleeping     11 awake
```

The serial camera is now a health-gated fusion source. A missing or stale
camera clears bit 4 in `valid` and sets bit 4 in `faults` after the boot grace
period. Audio and vision inference results expire after three seconds, so stale
model output cannot hold an alarm indefinitely. `vision.fe=1` means the current
successful frame is eligible for fusion; failed or incomplete frames use `0`.

Send `{"cmd":"fusion_calibrate_empty"}` while the bed is empty to rebuild the
pressure baseline from the next 20 distinct pressure frames. Calibration clears
the occupancy history; do not run it while an infant or test dummy is on the mat.

### Safety test

```json
{"cmd":"safety_test"}
```

This forces state 255 and produces a reliable `0x13` event. It is the preferred
end-to-end factory test for ESP32 receive, server queue, and ACK behavior.

## 9. Complete test vectors

The following frame sends `{"cmd":"safety_test"}` with transport sequence 1:

```text
AA 55 01 20 00 01 00 15
7B 22 63 6D 64 22 3A 22 73 61 66 65 74 79 5F 74 65 73 74 22 7D
58 3D
```

The following frame acknowledges safety event ID 1 with transport sequence 2:

```text
AA 55 01 22 00 02 00 04 00 00 00 01 32 46
```

The following frame requests a silent reset with transport sequence `0x1234`:

```text
AA 55 01 20 12 34 00 16
7B 22 63 6D 64 22 3A 22 73 69 6C 65 6E 74 5F 72 65 73 65 74 22 7D
D0 CF
```

All CRC values include the `AA 55` magic bytes.

## 10. ESP32 gateway behavior

Minimum implementation:

1. Configure the UART and start a persistent receive task before Wi-Fi setup.
2. Send type `0x20` payload `gateway_ready` every 2 seconds; do not wait for a reply before receiving RA8 status.
3. Parse and CRC-check frames into a bounded queue.
4. Treat a valid `0x10` at least once every 2 seconds as RA8 online.
5. Parse only needed JSON fields and ignore unknown fields.
6. ACK every valid `0x13` immediately after placing it in durable/local delivery state.
7. Deduplicate server events by the 32-bit `event` ID.
8. Reassemble `0x11` only if the server needs the complete infrared matrix.
9. Never forward ESP32 camera/audio through this UART; send those directly to the server.
10. Forward RA8 `vision`, model outputs, safety state, sensor health, `link`, and selected sensor summaries.
11. Keep network reconnect and UART parsing independent so loss of Wi-Fi cannot block UART.
12. If RA8 is offline for 10 seconds after the boot grace period, pulse the
    open-drain GPIO10-to-P712 request for 150 ms; enforce the 60-second
    automatic cooldown.
13. For `soother.set`, require a boolean `on` argument and map it to
    `soother_on` or `soother_off`.

Recommended server record for each status update:

```json
{
  "device_id": "configured-on-esp32",
  "esp32_time_ms": 0,
  "ra8_tick": 0,
  "safety": {},
  "thermal_summary": {},
  "bed_temperature": {},
  "environment": {},
  "pressure_summary": {},
  "microphone_inference": {},
  "sensor_health": {}
}
```

The server-facing schema should be versioned separately from this UART
protocol. Do not expose raw RA8 field names as a permanent cloud API.

## 11. Reference implementations

PC reference files in this repository:

```text
tools/ra8_esp32_protocol/ra8_protocol.py
tools/ra8_esp32_protocol/esp32_simulator.py
tools/ra8_esp32_protocol/ra8_live_monitor.py
```

`ra8_protocol.py` is the authoritative executable reference for frame parsing,
CRC, thermal chunk decoding, and pressure decoding.

## 12. ESP32 acceptance tests

The ESP32 adaptation is complete only when all of these pass:

1. All test vectors in section 9 decode with valid CRC values.
2. ESP32 receives valid `0x10` JSON continuously for at least 10 minutes with no parser reset.
3. Disconnecting RA8 TX causes an offline indication within 2 seconds; reconnecting recovers without reboot.
4. Sending `safety_test` produces `0x13`, ESP32 returns `0x22`, and the next status reports `safe.pending == 0` with `safe.acks` incremented.
5. Deliberately withholding ACK causes the same safety event ID to repeat about every 2 seconds.
6. A complete `0x11` matrix is assembled only from chunks sharing one thermal sequence.
7. Unknown JSON fields and unknown message types are ignored without dropping subsequent valid frames.
8. Wi-Fi disconnection does not stop UART reception or safety ACK handling.
9. Online `silent_reset` returns `0x21` with the matching
   `request_sequence`, then RA8 restarts without playing `BOOT.WAV`.
10. With UART deliberately disconnected while CPU0 still runs, one 150 ms
    GPIO10-to-P712 pulse restarts RA8 without playing `BOOT.WAV`.
11. A full power cycle still plays `BOOT.WAV`, and P4 GPIO10 never actively
    drives P712 high.
12. A deliberately halted CPU0 is documented as outside the P712 path and is
    recovered only by a hardware watchdog or true RESET_n.

## 13. Environment thresholds v2 and independent incidents

RA8 remains authoritative for the active threshold set. New gateways and
servers send this ASCII command payload:

```text
environment_thresholds_v2:<co2_max>,<temp_max_milli_c>,<rh_max_milli_percent>,<gas_min_ohm>,<body_temp_max_deci_c>,<rh_min_milli_percent>
```

Status JSON reports both the legacy `fusion.limits` array and the new array:

```text
fusion.limits2 = [co2_max, temp_max, rh_max, gas_min, body_temp_max, rh_min, revision]
```

The legacy command and `limits` remain accepted for one compatibility cycle;
legacy readers use a 30 percent RH lower default. Incident enum values are
append-only: `17=CO2 high`, `18=room temperature high`, `19=humidity high`,
`20=humidity low`, `21=suspected VOC high`, and `22=visual subject hidden`.
Environment incidents require confirmed bed presence, fresh valid samples,
10 seconds beyond the threshold and 30 seconds inside the hysteresis band to
clear. They are independent events and must not be collapsed into a single
generic air-quality notification.

## 14. Dedicated story UART handshake

Story audio bulk transfer uses a separate 3.3 V UART at 1000000 baud, 8N1:
P4 GPIO50 TX to RA8 P715 RX, and RA8 P714 TX to P4 GPIO51 RX. RA8 sends an
`SBL2` type `READY(6)` header every two seconds. P4 must receive READY and
complete `PING(1)/ACK(0x80)` before `BEGIN/DATA/END`. The receiver performs
rolling byte synchronization on `SBL2`; malformed or shifted bytes do not
require a board reset. Only three consecutive dedicated-link failures permit
the legacy main-UART fallback, and the fallback reason is exposed in gateway
telemetry.
