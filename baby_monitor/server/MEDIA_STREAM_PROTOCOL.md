# ESP32-S3 real-time media protocol

## Version 1 scope

Version 1 enables independent low-latency JPEG video and PCM16 microphone
monitoring. Media is relayed through bounded in-memory queues and is not written
to telemetry JSON, SQLite, or the artifact store. The existing HTTP JPEG/MJPEG
endpoints remain available for older gateways.

## WebSocket endpoints

### Device audio upload

`WSS /api/v1/media/device/audio`

The ESP32-S3 opens this outbound connection and supplies the existing
`X-Device-ID` and `X-Device-Key` headers during the WebSocket handshake. The
server binds the authenticated device ID to the connection, so media frames do
not contain a device ID.

The device sends one complete S3MD audio frame in each binary WebSocket
message. It may send `media.heartbeat` text messages while capture is paused.
The server does not acknowledge individual audio frames.

### Device video upload

`WSS /api/v1/media/device/video`

Authentication is identical to the audio endpoint. Each binary WebSocket
message contains one complete `S3MD` header followed by one JPEG. The server
validates the header, dimensions, length, JPEG SOI/EOI markers and then retains
only the latest frame.

### Browser audio subscription

`WSS /api/dashboard/media/{device_id}/audio/ws`

This endpoint uses the same dashboard HTTP Basic authentication as the rest of
the website, and the application validates it during the WebSocket handshake.
Deployments using the provided Nginx template enforce it at the gateway as
well. Every browser receives an independent bounded queue. Closing the page,
changing the selected device, or pressing Stop immediately removes the
subscription.

### Browser video subscription

`WSS /api/dashboard/media/{device_id}/video/ws`

Authentication and heartbeat behavior match the browser audio endpoint. Each
message is the original complete `S3MD + JPEG` device frame. The browser queue
has capacity one, so congestion replaces stale frames instead of increasing
latency. The compatibility MJPEG endpoint remains
`GET /api/dashboard/media/{device_id}/video`.

## S3MD binary header

The header is exactly 32 bytes. Header integers use network byte order (big
endian). PCM payload samples use little endian.

| Offset | Size | Field | Audio v1 value | Video v1 value |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | magic | ASCII `S3MD` | ASCII `S3MD` |
| 4 | 1 | version | `1` | `1` |
| 5 | 1 | media_type | `2` for PCM16LE | `1` for JPEG |
| 6 | 2 | flags | bit 0 = discontinuity | bit 0 = discontinuity |
| 8 | 4 | sequence | unsigned frame sequence, wraps at 2^32 | same |
| 12 | 8 | timestamp_us | monotonic capture timestamp in microseconds | same |
| 20 | 4 | payload_length | `640` | JPEG bytes, `1..524288` |
| 24 | 2 | meta0 | sample rate: `16000` | width |
| 26 | 2 | meta1 | samples per frame: `320` | height |
| 28 | 2 | meta2 | channels: `1` | ESP JPEG quality, `0..63` |
| 30 | 2 | meta3 | bits per sample: `16` | reserved, must be `0` |

Each audio frame represents 20 ms. The binary WebSocket message must therefore
be exactly 672 bytes: a 32-byte header followed by 640 bytes containing 320
signed PCM16LE samples.

The discontinuity flag must be set on the first frame after capture starts,
after an input overrun, or after a sequence/timestamp reset. The browser clears
its jitter buffer when it sees this flag.

Malformed frames cause a `media.error` text message followed by WebSocket close
code `1003`. Authentication failure uses close code `1008`.

## Device control messages

The server sends UTF-8 JSON text messages on the device upload WebSocket. A
control message is sent immediately after connection, whenever the browser
subscriber count changes, and every 10 seconds while the upload connection is
idle. The periodic refresh is intentional: it repairs a lost control message
without requiring the browser or the S3 to reconnect.

```json
{
  "type": "media.control",
  "version": 1,
  "sequence": 1,
  "enabled": true,
  "subscribers": 1,
  "target_fps": 0
}
```

For the audio channel, `target_fps` is reserved and is always zero. For video it
is currently `8` and the S3 clamps it to its supported range. When `enabled` is
false, the S3 stops that media capture and clears its pending queue, but keeps
the WSS connection and heartbeat active. Capture resumes when the first browser
subscribes and pauses after the final subscriber leaves.

RA8 detection boxes are not inserted into `S3MD/1` video frames and the server
does not re-encode JPEG. Dashboard/mobile clients read the latest
`vision_overlay` object from their snapshot/SSE stream and draw its normalized
target-camera coordinates over the decoded S3 image. This keeps media latency
independent from RA8 inference latency and lets clients hide stale overlays.

Recommended device heartbeat:

```json
{"type":"media.heartbeat","version":1,"sequence":42}
```

The response is `media.heartbeat_ack` with the same sequence and server time.

## Backpressure and latency

- The S3 audio queue is limited to eight frames (160 ms) and drops the oldest
  frame when full.
- Every browser subscriber has an independent server queue limited to eight
  frames. The server also drops the oldest frame when full and never blocks the
  device receiver.
- The AudioWorklet starts with three buffered frames and supports at most eight
  frames. It resamples from 16 kHz to the browser audio device rate.
- The page reports sequence gaps, AudioWorklet underruns, dropped samples,
  buffer duration, and relative transport delay. Because the S3 timestamp is
  monotonic rather than UTC, displayed delay is relative to the shortest path
  observed during the current connection, not an absolute clock-synchronized
  one-way measurement.
- Audio WSS is independent of the RA8 telemetry connection. Failure or
  congestion on either connection must not block the other.
- The S3 video capture queue and every browser video queue retain one latest
  frame. Slow consumers therefore lose frames but do not accumulate delay.

## Browser and deployment behavior

The browser opens the audio WebSocket only after a user presses Start, which
also satisfies browser audio autoplay policy. It sends a heartbeat every 20
seconds, reconnects with bounded exponential backoff, and releases the
subscription when stopped, when the selected device changes, or when the media
tab is left.

The S3 device must reconnect after both transport failures and clean server
closes. With `esp_websocket_client`, set `enable_close_reconnect = true` in
addition to leaving automatic reconnect enabled; otherwise a normal server
restart leaves telemetry online while the independent media upload channel
stays disconnected until the S3 is rebooted.
On every `WEBSOCKET_EVENT_CONNECTED`, the S3 must clear its previous
`media.control` sequence baseline. It must also clear the baseline on
`CLOSED`, `DISCONNECTED`, and transport error, because a restarted server
begins a new control sequence at 1.

Nginx must forward WebSocket Upgrade headers, disable proxy buffering and
request buffering, and use long read/send timeouts for all four media WSS
endpoints. Video and audio use separate connections, queues and upload tasks.
