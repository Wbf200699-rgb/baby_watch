# ESP32-S3 RA8 gateway

This ESP-IDF project bridges the RA8 UART protocol to the server and hosts the
optional preview camera/microphone streams.

1. Copy `main/gateway_secrets.example.h` to `main/gateway_secrets.h` and fill in
   local Wi-Fi and device credentials. The real file is ignored by Git.
2. Build with ESP-IDF 5.x: `idf.py set-target esp32s3`, then `idf.py build`.
3. Flash with `idf.py -p COMx flash monitor`.

Firmware 3.3.0 starts the UART reader before Wi-Fi/media initialization and
sends `gateway_ready` every two seconds. RA8 waits for that handshake before
bulk telemetry, preventing cold-boot UART ring-buffer overflow.

Real-time RA8 status now uses the authenticated persistent
`/api/v1/device/stream` WSS. A fixed 500 ms producer writes one latest-value
slot, while a dedicated high-priority sender transmits status and commands.
Network latency therefore does not extend the 2 Hz production period, and
congestion cannot create an unbounded telemetry backlog. Only one telemetry
frame may await a server ACK;
after a 4-second timeout only the newest slot is retried on the same connection.
The WSS is rebuilt only after three consecutive ACK timeouts. Camera and
microphone keep their independent WSS connections. Every media reconnect
establishes a new control-sequence session, including a clean server `CLOSE`;
stale sequence numbers therefore cannot leave capture permanently paused.
Both media channels accept fragmented control text, send a 15-second
application heartbeat while idle, and mark the first frame after a reconnect,
capture resume, or queue overrun as discontinuous.

Pressure and thermal matrices use a second authenticated
`/api/v1/device/matrices` WSS with independent reconnect, ACK state, task and
preallocated buffers. Pressure targets 4 Hz and full 80x64 thermal targets
2 Hz. Each kind keeps only the latest complete RA8 frame and uses its own
contiguous upload sequence. A lost ACK retries the identical retained frame,
so matrix congestion cannot block or restart telemetry.
Telemetry contains compact sensor summaries only, including the CPU1 SD-card
diagnostic scalars carried in the RA8 microphone status object.
Pressure/thermal samples use their independent binary channel. The independent
producer stays on its 500 ms clock, and congestion drops stale snapshots
instead of slowing RA8 sampling.
Because telemetry itself keeps the socket active, protocol PING is only a
5-minute fallback and an application heartbeat is suppressed while the
producer is healthy.

At boot, the gateway reserves its telemetry tasks and establishes the priority
device WSS, then starts the independent matrix WSS before microphone and camera
WSS. The microphone handshake is given a bounded head start before video, and
mbedTLS working buffers are explicitly allocated from the N16R8's 8 MiB PSRAM.
This prevents simultaneous media handshakes from exhausting internal SRAM and
leaving one uploader in a permanent reconnect loop.
