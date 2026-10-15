# ESP32-P4X-EYE 与服务器协议 v3

## 1. 设计边界

- ESP32-P4X-EYE 使用四条相互独立的长连接：遥测/命令、矩阵、视频和音频 WSS。
- ESP32 优先上传设备状态、推理结果、警报和小体量日志。
- 训练用照片、音频和连续原始矩阵写入 RA8/ESP32 的 SD 卡，不持续上传服务器。
- 网页实时预览只保留“最新一帧”矩阵：压力最高 8 Hz，热成像最高 4 Hz；允许丢旧帧，不作为训练数据源。
- 服务器通过命令队列独立控制四类数据的开始和停止。
- HTTP 请求头必须包含 `X-Device-ID` 和 `X-Device-Key`。

## 1.1 独立矩阵 WebSocket

设备主动连接：

```text
WSS /api/v1/device/matrices
X-Device-ID: esp32cam-01
X-Device-Key: <device secret>
```

该连接和 `/api/v1/device/stream` 必须同时存在，不能因 `device_id` 相同互相替换。
设备在线状态仍由遥测连接和最近遥测时间判断；矩阵连接只更新
`matrix_connected`、`last_pressure_at`、`last_thermal_at`、两类 FPS 和丢帧计数。

每条二进制消息由固定 14 字节网络字节序头和 little-endian `int16` 负载组成：

```text
>4sBBHHI
```

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | ASCII `RA8M` |
| 4 | 1 | 版本 `1` |
| 5 | 1 | `1=thermal`，`2=pressure` |
| 6 | 2 | rows，大端 |
| 8 | 2 | cols，大端 |
| 10 | 4 | sequence，大端无符号整数 |
| 14 | N | `rows*cols` 个 little-endian `int16` |

P4X-EYE 当前发送以下尺寸；服务器暂时保留 4.2.0 错误头兼容：

- thermal：规范线格式 `rows=64, cols=80`，负载 10240 字节，完整帧 10254 字节；负载按64行、每行80点排列且不转置；P4X-EYE 4.2.0 曾错误发送 `rows=80, cols=64`，服务端仅为迁移期修正其元数据；
- pressure：`16x16`，负载 512 字节，完整帧 526 字节。

单帧上限为 12 KiB。magic、版本、尺寸、负载长度或序号非法时，服务器发送错误并关闭该
矩阵连接。`device_id` 只来自鉴权头，不能编码进二进制帧。
pressure 和 thermal 各自维护上传序号；每成功提交一帧加 1，不能直接复用高频 RA8 源帧
序号，否则服务端会把主动降采样误计为网络丢帧。ACK 丢失时可用同一序号重发，重连后允许
从任意序号重新开始。

服务端完成校验和最新帧缓存更新后立即回复，不等待 SQLite、训练采集或浏览器：

```json
{
  "kind": "matrix_ack",
  "payload": {
    "kind": "pressure",
    "sequence": 123,
    "received_at": "2026-07-28T08:00:00.000+00:00"
  }
}
```

服务端对每个 `device_id/kind` 分别维护容量为 2 的广播和持久化队列。队列满时丢弃旧帧、
保留最新帧。普通运行时每类矩阵最多每秒写入 SQLite 一帧；对应训练采集开启时按收到的
最新帧保存。慢数据库和慢浏览器不得阻塞 ACK。

矩阵连接没有应用层空闲关闭定时器；反向代理读超时至少 300 秒，连接存活由 WebSocket
ping/pong 判断。服务端状态输出每类接收 FPS、ACK P95、数据库写入 P95、序号缺口、两条
后台队列深度、丢帧、最后关闭码和重连次数。

音视频状态中的 `video_diagnostics` / `audio_diagnostics` 还输出接收 FPS、帧龄、序号缺口、
订阅队列深度、慢订阅者丢帧、重连次数和最近关闭码。媒体不进入 SQLite，也没有应用层
逐帧 ACK，因此这两类状态不提供数据库写入耗时或 ACK P95。

## 1.2 兼容 HTTPS 批次通道

旧固件复用一条 TLS keep-alive 连接，每 500 ms 调用
`POST /api/v1/batch`。每个请求只包含发送时刻的最新数据；网络忙或断开时允许跳过中间帧，
恢复后不补发历史矩阵。该接口及 `/api/v1/device/stream` 中的二进制矩阵至少保留一个版本
周期；新固件使用 1.1 节的独立矩阵 WSS。
固件必须复用同一个长期存活的 `HTTPClient` 实例；仅复用 `WiFiClientSecure` 而每次新建
`HTTPClient` 会被 ESP32 Arduino Core 的析构函数关闭套接字，实际仍会重复 TLS 握手。

批次 v2 是一个 48 字节小端头，后接遥测 JSON、压力矩阵、热成像矩阵、JPEG 和
PCM。服务端继续接受原来的 24 字节 v1 包，因此旧固件不会因服务端升级而失效。

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | ASCII `RA8B` |
| 4 | 1 | 版本 `2` |
| 5 | 1 | flags：bit0 压力，bit1 热成像，bit2 JPEG，bit3 PCM |
| 6 | 2 | 遥测 JSON 字节数，小端 |
| 8 | 4 | 压力序列号，小端 |
| 12 | 4 | 热成像序列号，小端 |
| 16 | 2 | 压力行数，小端 |
| 18 | 2 | 压力列数，小端 |
| 20 | 2 | 热成像行数，小端 |
| 22 | 2 | 热成像列数，小端 |
| 24 | 4 | JPEG 序列号，小端 |
| 28 | 4 | JPEG 字节数，小端 |
| 32 | 4 | PCM 序列号，小端 |
| 36 | 4 | PCM 字节数，小端 |
| 40 | 4 | PCM 采样率，小端 |
| 44 | 1 | PCM 声道数 |
| 45 | 1 | PCM 位深，当前为 `16` |
| 46 | 2 | 保留，写零 |
| 48 | J | UTF-8 遥测 JSON |
| 48+J | P | 可选压力 `rows * cols` 个 little-endian `int16` |
| 48+J+P | T | 可选热成像 `rows * cols` 个 little-endian `int16` |
| 48+J+P+T | V | 可选 JPEG |
| 48+J+P+T+V | A | 可选 little-endian signed PCM16 |

兼容批次中的压力保持 16x16，最高 2 Hz；旧固件可将 80x64 热成像做 2x2 平均后发送
40x32，最高 1 Hz。独立矩阵 WSS 始终发送完整 80x64 热成像。远程视频约每 2 秒发送一张
QVGA JPEG；ESP32 预览
音频降采样为 8 kHz 单声道 PCM16。RA8 本地模型仍使用完整精度、尺寸和采样率。命令按 `id`
去重，通过独立 HTTPS 轮询和 ACK 确认；警报事件使用可靠队列，不采用矩阵的丢帧策略。

## 2. 设备上线

`POST /api/v1/device/hello`

```json
{
  "protocol_version": 1,
  "firmware": "esp32cam-0.1.0",
  "hardware": "ESP32-CAM",
  "capabilities": ["camera", "audio", "thermal", "pressure", "sd_collection"]
}
```

## 3. 状态与推理结果

P4X-EYE 使用 `/api/v1/device/stream` 中的 `telemetry` 文本帧，每 250 ms 一次（4 Hz）。
服务器更新内存最新状态后立即返回 `telemetry_ack`；SSE 广播和 SQLite 走容量为 2 的独立
后台队列。普通历史记录最多写入 1 Hz，温度训练采集开启时才保存更高频数据。旧设备仍可
使用 `POST /api/v1/telemetry`。禁止在遥测中放矩阵、图像或 PCM。

```json
{
  "protocol_version": 1,
  "sequence": 42,
  "device_time_ms": 12000,
  "ra8_tick": 11800,
  "wifi_rssi": -51,
  "firmware": "esp32cam-0.1.0",
  "sensors": {
    "thermal_valid": 5120,
    "pressure_active": 12,
    "temperature_c": 24.3,
    "environment": {
      "seq": 12,
      "s": 3,
      "co2": 735,
      "t_mc": 24620,
      "rh_mp": 51340,
      "ready": 1,
      "crc_err": 0,
      "i2c_err": 0,
      "to": 0,
      "rec": 0,
      "fsp": 0,
      "last": 123456
    }
  },
  "inference": {
    "state": "sleeping",
    "confidence": 96.2,
    "cry": false,
    "face_covered": false,
    "edge_risk": false
  },
  "health": {
    "sd_ready": true,
    "sd_free_mb": 14820,
    "active_kinds": ["audio", "thermal"]
  }
}
```

ESP32 maps the complete RA8 status `fusion` object into `inference` without
renaming or dropping its compact arrays. It also maps `safe` to top-level
`safety` and forwards top-level `vision` unchanged. Preserve the legacy numeric
`state`, `candidate`, `confidence`, `severity`, `sources`, `valid` and `faults`
values for old clients. New clients use `ctx`, `alarm` and `ai`; they must not
reduce the fusion result back to one mutually exclusive state.

Fusion schema v2 additions are:

```json
{
  "inference": {
    "sv": 2,
    "ev": 381,
    "ctx": [1,1,1,1,1,1,0,0,1,1,0],
    "alarm": [2,7,19,2,2,132],
    "pv": [612,88,14,-3],
    "vv": [77,210,1,1,1,120],
    "drop": 0,
    "ai": [[7,2,2,3,82,24,19,123456]]
  }
}
```

- `ctx` order is presence, arousal, bed motion, airway view, edge state,
  distress, cry-cause hint, body temperature, bed thermal, caregiver and
  system health.
- `alarm` is global level, primary type, primary incident ID, primary
  lifecycle, active count and active-type bit mask.
- `ai[]` entries are type, lifecycle, level, severity, confidence, source mask,
  incident ID and since-tick.
- Full enum tables are authoritative in
  `tools/ra8_esp32_protocol/ESP32_ADAPTER_PROTOCOL.md`.

Fusion schema v3 retains the v2 arrays above and adds the environment/SCD41
source and fault at bit 5. The gateway maps the RA8 top-level `scd` object to
`sensors.environment` without changing its 12 compact fields. `co2` is ppm,
`t_mc` is milli-degrees Celsius, `rh_mp` is milli-percent RH, and `last` is the
CPU0-translated RA8 tick of the last CRC-valid sample, in the same tick domain
as the top-level `ra8_tick`. `ready` is only the result of the latest
data-ready poll; it is not a health bit. Old firmware may omit this optional
object.

Fusion schema v6 keeps every earlier compact-array prefix stable and extends
`inference.ex` from 17 to 21 entries. The appended values are
`thermal_bed_x1000`, `thermal_edge_x1000`,
`position_agreement_x1000`, and `calibration_profile_version`. Servers and
clients must accept the old 17-entry form. The infrared hotspot value at
`ex[11]` is in 0.1 degree Celsius despite its legacy firmware field name.

Fusion schema v8 keeps the v7 `inference.comfort` fields and adds the
`suspected_fever` incident without changing any earlier field:

- legacy state `13` means `suspected_fever`;
- incident type `16` means `suspected_fever`;
- `ctx[7]` changes from `unknown` only when infant presence and a valid thermal
  hotspot are both available;
- `ex[11]` remains the screened hotspot mean in 0.1 degree Celsius units.

Fusion schema v7 added `inference.comfort` without changing any earlier field:

```text
[action_mask, reason_mask, confidence, requires_confirmation, block_mask]
```

The server decodes this into `fusion.comfort` for web and mobile clients. It is
advisory only; no server or gateway background task may execute an actuator
from this field.

The server validates the SCD41 physical encoding range but deliberately does
not invent a CO2 alarm threshold. Environment freshness uses the status,
sequence and last-good time, while multimodal fusion reports an environment-only
failure as degraded health rather than a blind core-monitoring path.

### 3.1 BME688 环境扩展

RA8 状态中的顶层 `bme` 紧凑对象由 P4 网关放到
`sensors.environment.bme688`，与原有 SCD41 字段并存：

```json
{
  "sensors": {
    "environment": {
      "seq": 12,
      "s": 3,
      "co2": 735,
      "t_mc": 24620,
      "rh_mp": 51340,
      "ready": 1,
      "last": 123456,
      "bme688": {
        "s": 3,
        "addr": 118,
        "t_mc": 24750,
        "rh_mp": 50820,
        "p_pa": 100825,
        "gas_ohm": 125400,
        "flags": 7,
        "last": 123450,
        "err": 0
      }
    }
  }
}
```

字段单位和语义：

| 字段 | 单位/编码 | 说明 |
|---|---|---|
| `s` | 枚举 | `0` 未初始化，`1` 启动，`2` 等待测量，`3` 样本正常，`4` 未找到，`5` 数据/驱动错误，`6` I²C 错误，`7` 超时，`8` 恢复中 |
| `addr` | 7-bit 地址 | BME688 为 `0x76` 或 `0x77` |
| `t_mc` | 0.001 °C | BME688 补偿温度 |
| `rh_mp` | 0.001 %RH | BME688 补偿相对湿度 |
| `p_pa` | Pa | BME688 补偿气压 |
| `gas_ohm` | Ω | BME688 原始气体传感电阻 |
| `flags.bit0` | 布尔 | `new_data`，本轮得到新样本 |
| `flags.bit1` | 布尔 | `gas_valid`，芯片声明气阻结果有效 |
| `flags.bit2` | 布尔 | `heater_stable`，气体加热器已稳定 |
| `last` | RA8 tick | 最近有效样本，与顶层 `ra8_tick` 同一时钟域 |
| `err` | 无符号整数 | 驱动累计错误次数；用于观察增量，历史累计值本身不单独代表当前故障 |

`gas_ohm` 只表示原始气阻。当前 RA8/P4/服务器链路没有运行 Bosch BSEC，
因此不得把该字段命名、换算或展示为 VOC 浓度、VSC、eCO₂ 或 IAQ。只有
`flags.bit1=1` 且 `flags.bit2=1` 时，客户端才应把气阻标为当前有效；加热器预热期间
温湿度和气压仍可显示，但气阻状态应为“预热/无效”。

为兼容早期 P4 适配版本，服务器也接收 `sensors.bme688`。服务器在校验后会在内存广播和
SQLite 历史入库前将它归一到 `sensors.environment.bme688`，不会同时输出两个副本。
若两个位置同时存在，规范位置的同名字段优先。旧固件完全省略 BME688 时，原有 SCD41
遥测、诊断和客户端行为不变。

RA8 视觉线程的原始校准结果放在顶层 `vision`，网关应原样转发，不能并入
`sensors` 或 `inference`。这个对象只包含数值结果，不包含 JPEG：

```json
{
  "vision": {
    "q": 18,
    "f": 7,
    "s": 6,
    "r": 7,
    "n": 7,
    "w": 320,
    "h": 240,
    "jb": 18342,
    "os": 0,
    "ml": 214,
    "t": [14, 6, 47, 18, 17, 102],
    "e": [0, 0, 0],
    "ss": [812, 744, 115, 0, 0],
    "oc": [0, 851, 851, 102, 47],
    "bh": [1, 734, 162, 734, 31, 73],
    "fe": 1,
    "dc": 1,
    "d": [[0, 812, 12, 18, 220, 180]]
  }
}
```

- `s` 是视觉运行状态，`r` 是三个模型的就绪位掩码，正常就绪为 `7`。
- `ss` 顺序为 infant、head_face、blanket、soft_object、caregiver。
- `oc` 为类别、置信度及 clear、partial、covered、not_visible 四类分数。
- `bh` 为姿态类别、置信度及 lying、sitting、crawling_kneeling、standing、unknown 五类分数。`standing` 只表示单帧姿态，床边风险必须再融合目标位置和压力边缘载荷。
- `d` 中每个框为 `[class_id, score_x1000, x, y, width, height]`，坐标对应
  `w × h` 原始 JPEG。
- `fe=1` 表示这一张成功推理帧可进入融合层；失败、未完成或质量不合格帧为 `0`。
- 网关必须保留 `d` 检测框，服务器使用设备标定矩阵把 RA8 串口摄像头坐标映射到
  ESP32-S3 预览画面。映射只改变显示坐标，不改变 RA8 本地决策。

RA8 到网关的状态帧仍以 4096 字节为上限。CPU0 先尝试完整诊断状态；若加入视觉结果后
超限，会依次省略未被服务器使用的压力原始通道、摄像头响应头、音轨列表，最后才省略
状态 JSON 内重复的压力矩阵。任何回退都必须保留核心传感器状态和 `vision`，避免超限时
整条遥测消失。

## 4. 独立训练采集

四种训练数据必须分别开始和停止，不能用一个会话同时采集多个通道：

- `temperature`: 从遥测中的 `sensors.bed_temperature` 独立归档到服务器。
- `thermal`: 从 64 x 80 热成像矩阵流独立归档到服务器。
- `pressure`: 从 16 x 16 压力矩阵流独立归档到服务器。
- `audio`: 服务器保存麦克风状态特征，同时下发命令让 RA8 将 WAV 写入 SD 卡。

温度、热成像和压力数据本来就在持续上传，其开始/停止由服务器的数据门控完成。
`audio` 和 `camera` 会产生下面的 `collection.start` 或 `collection.stop`，分别控制
RA8 SD 卡上的 WAV 和 JPEG 采集。

服务器优先在每次 `POST /api/v1/batch` 的响应中附带 `command`，所以媒体预览等
控制命令通常在下一个 500 ms 遥测周期内到达。`GET /api/v1/commands/poll` 仅作为
批量链路异常时的低频后备。无命令时 `command` 为 `null`。

```json
{
  "ok": true,
  "command": {
    "id": 17,
    "delivery_attempt": 1,
    "command": "collection.start",
    "args": {
      "session_id": "session_0123456789abcdef",
      "name": "dummy_sleep_01",
      "labels": ["sleeping", "covered"],
      "storage": "sd",
      "kinds": ["audio"]
    }
  }
}
```

设备命令的 `kinds` 为单元素 `["audio"]` 或 `["camera"]`。ESP32 分别映射为 RA8
`mic_record`/`mic_record_stop` 或 `camera_record`/`camera_record_stop`。各采集通道互不联动。

网页“录制测试”页还会下发同样的结构，并额外使用：

```json
{
  "command": "collection.start",
  "args": {
    "kinds": ["audio"],
    "storage": "sd",
    "purpose": "media_test",
    "filename": "cry0001"
  }
}
```

`filename` 是可选的无扩展名 basename，只允许 1–8 个 ASCII 字母、数字、下划线
或连字符。网关必须原样映射为以下 RA8 文本命令，不能把扩展名或路径传入 RA8；
CPU1 落盘时会统一转成大写，确保只创建 FAT 8.3 短目录项：

- 音频开始：`mic_record:cry0001`
- 音频停止：`mic_record_stop`
- 摄像头连续开始：`camera_record:cam0001`
- 摄像头停止：`camera_record_stop`
- 摄像头单张：`collection.capture` 映射为 `camera_capture:cam0001`

没有 `filename` 时仍发送原来的无冒号命令并由 RA8 自动编号。同名文件不会覆盖，
RA8 CPU1 会依次尝试原名及压缩到 8 字符的数字后缀名。设备遥测应保留 RA8 `mic.file` 和
`cam.file`，分别映射到 `sensors.microphone.file` 和 `sensors.camera.file`，供网页显示
最终实际文件名。

命令采用至少一次投递。ESP32 未确认时，服务器约 5 秒后会用相同 `id`
重新投递，并递增 `delivery_attempt`。ESP32 必须按命令 `id` 去重，重复命令应返回
第一次执行的结果，不能再次创建文件或重复切换状态。

`soother.set` 是幂等的保持型电机开关命令，参数为 `{"on": true|false}`。
P4 分别映射为 `soother_on` 或 `soother_off` 后发给 RA8；RA8 在 P914 保持目标电平。
服务端 ACK 只表示 P4 已转发，最终完成以遥测
`sensors.soother.on` 等于目标值为准。

RA8 融合协议 v7 在 `inference.comfort` 提供
`[action_mask, reason_mask, confidence, requires_confirmation, block_mask]`。
P4 只需随完整 `fusion` 对象透明转发，不能据此自行操作 GPIO。服务端在网页和移动快照中
解码为 `fusion.comfort`，并将它作为建议显示。只有用户明确操作后才下发
`light.set` 或 `soother.set`。两种动作都必须由用户确认；服务端后台任务和 AI 解释层
不得自动驱动继电器。

- `collection.start`: 向 RA8 请求打开新的 WAV 并开始写入。
- `collection.stop`: 向 RA8 请求刷新缓存、修正 WAV 头并关闭文件。
- `collection.capture`: 仅用于摄像头单张 JPEG，不开启连续采集。
- 摄像头 start 后按帧保存 `/CAMERA/Cxxxxxxx.JPG`，stop 后不再开始下一帧。
- 同一通道重复 start/stop 必须幂等。
- 设备重启后应读取 SD 上的状态文件，将未正常关闭的会话标为 `interrupted`，不要自动继续录制。
- `media.preview.start`: ESP32 本地开启限时公网预览；参数为 `video`、`audio` 和
  `ttl_seconds`，不转发给 RA8。
- `media.preview.stop`: 立即停止公网音视频上传，不影响遥测和本地模型。

执行后：`POST /api/v1/commands/{id}/ack`。当前 ACK 只证明 ESP32 已把命令转发给
RA8，不证明 SD 文件已创建。服务器必须继续检查麦克风遥测中的 `wst`、`werr`、
`wopen`、`wwr` 和 `wbytes`；正式采集应以 `wst=6`（文件打开）或 `wst=7`
（正在写入）为准。

```json
{
  "ok": true,
  "result": {
    "session_id": "session_0123456789abcdef",
    "forwarded": true,
    "ra8_command": "mic_record"
  }
}
```

### 4.1 AI 故事音频下发

手机创建故事后，服务器在后台完成文本生成和 TTS。音频必须为 PCM16LE、单声道、
16 kHz WAV，且不超过 4 MiB。生成和合成不在遥测或矩阵 WSS 协程中执行。

WAV 就绪后，服务器通过现有命令通道下发：

```json
{
  "id": 73,
  "command": "story.audio.play",
  "args": {
    "story_id": "32位十六进制ID",
    "path": "/api/v1/ai/stories/32位十六进制ID/audio",
    "filename": "AI12AB34.WAV",
    "size_bytes": 960044,
    "sha256": "64位小写十六进制",
    "crc32": 305419896,
    "autoplay": true
  }
}
```

P4 使用设备鉴权头从同一服务器 HTTPS 下载 `path`，并严格校验长度、SHA-256、CRC32 和
WAV 格式。下载和 RA8 文件传输在容量为 1 的独立后台队列执行，不能占用实时遥测、矩阵
或媒体任务。重复命令 ID 必须幂等；同一时刻只允许一个故事传输。

P4 与 RA8 继续复用现有 921600 baud 全双工 UART，不新增接线。外层帧保持：

```text
AA 55 | VER=01 | TYPE | SEQ_BE16 | LEN_BE16 | PAYLOAD | CRC16_BE16
```

CRC 为 CRC16/CCITT-FALSE：poly `0x1021`、init `0xFFFF`、不反射、xorout `0`。
RA8 将完成的故事原子保存到 `/sd0/STORY/AIxxxxxx.WAV`；`/sd0/MUSIC` 仅保存普通音乐并
参与上一首/下一首扫描，因此故事不会再被音乐切换命令选中。
故事消息类型为 `0x30` BEGIN、`0x31` CHUNK、`0x32` END；所有多字节控制字段为大端，
文件字节保持原样。

BEGIN payload 固定 29 字节：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 1 | story protocol version=`1` |
| 1 | 1 | flags，bit0=`autoplay` |
| 2 | 2 | reserved=`0` |
| 4 | 4 | transfer_id |
| 8 | 4 | total_bytes |
| 12 | 4 | whole-file CRC32 |
| 16 | 1 | filename_length=`12` |
| 17 | 12 | `AI[0-9A-F]{6}.WAV` |

CHUNK payload 为 `20 + chunk_length`，`chunk_length <= 448`：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 1 | version=`1` |
| 1 | 3 | reserved=`0` |
| 4 | 4 | transfer_id |
| 8 | 4 | byte_offset |
| 12 | 2 | chunk_length |
| 14 | 2 | reserved=`0` |
| 16 | 4 | chunk CRC32 |
| 20 | N | WAV bytes |

END payload 固定 16 字节：version、flags、reserved、transfer_id、total_bytes、whole-file
CRC32，布局与 BEGIN 前 16 字节一致。

RA8 使用现有 `0x21 CMD_RESULT` 返回固定 16 字节结果：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 1 | version=`1` |
| 1 | 1 | operation：1=BEGIN、2=CHUNK、3=END |
| 2 | 1 | result：0=OK，1..8 为参数/SD/顺序/CRC/写入/完成/BUSY 错误 |
| 3 | 1 | status：0=idle、1=receiving、2=ready、3=playing、4=complete、5=error |
| 4 | 4 | transfer_id |
| 8 | 4 | next_offset |
| 12 | 2 | 对应请求的 UART sequence |
| 14 | 2 | reserved=`0` |

P4 仅在收到 END 的 `result=0`、`next_offset=total_bytes` 后，才向服务器确认命令成功。
这条命令的成功语义强于普通 `forwarded to RA8`：表示 CPU1 已在 FAT 锁内写完临时文件、
验证整文件 CRC、原子重命名并加入播放队列。播放是否结束仍以 RA8 状态中的
`aud.story.s` 为准。任何失败都必须删除 `.TMP`，不能播放半文件。

### 4.2 RA8 硬件阈值同步

环境和疑似发热阈值由 RA8 融合层统一执行。网页保存后，服务器通过普通
`ra8.command` 原样下发以下 ASCII payload：

```text
environment_thresholds:<CO2_PPM>,<TEMP_MILLI_C>,<RH_MILLI_PERCENT>,<GAS_MIN_OHM>,<BODY_TEMP_DECI_C>
```

例如：

```text
environment_thresholds:2000,30000,70000,10000,375
```

RA8 必须整组校验后原子替换，不能只更新部分字段。实际生效值通过
`inference.fusion.limits=[co2,temp,rh,gas,body,revision]` 回报。服务端以该硬件回报作为
告警和页面显示的权威值；无该字段的旧固件才回退使用服务端保存值。设备重新建立 hello
连接时，服务端会重新下发已保存配置。

## 5. 音频 SD 卡目录

```text
/MIC/
  M0000001.WAV
  M0000002.WAV
  CRY0001.WAV
  CRY0_001.WAV
```

WAV 为 16 kHz、单声道、PCM16。训练会话未指定自定义文件名时仍需按时间与导出的
会话清单对应；录制测试页会直接显示 RA8 最终采用的文件名。

## 6. 警报

`POST /api/v1/events`，`event_id` 在单设备内递增，服务器按设备和 event_id 去重。

```json
{"event_id":8,"event_type":"face_covered","severity":4,"confidence":94.1,"sources":["camera","thermal"],"payload":{}}
```

## 7. 模型放回 SD 卡

```text
BABY_MODELS/
  cry_v3/
    manifest.json
    model.tflite
```

manifest 应包含 `model_id`、`version`、`target`、输入规格、类别表、量化参数和模型文件 SHA-256。RA8/ESP32 只加载目标匹配且校验通过的模型。

## 8. 实时媒体预览

实时预览使用有界内存中继，不写入 SQLite，也不替代 SD 训练数据：

- `POST /api/v1/media/video?sequence=N`：`Content-Type: image/jpeg`，单帧不超过 512 KiB。
- `POST /api/v1/media/audio?sequence=N&sample_rate=16000&channels=1&bits=16`：有符号小端 PCM16，单块不超过 128 KiB。

经典 ESP32 默认关闭公网音视频上传，以免 TLS 请求阻塞 500 ms 状态遥测。
实验时优先使用设备的局域网 MJPEG/WebSocket 地址。只有明确需要公网预览并确认
遥测仍能稳定达到目标频率时，才打开云媒体上传。

实时矩阵使用：

- `POST /api/v1/matrices/thermal?sequence=N&rows=64&cols=80&unit=deci_c`
- `POST /api/v1/matrices/pressure?sequence=N&rows=16&cols=16&unit=raw`
- 请求体为小端 `int16`，服务器保存有限历史并通过 SSE 直接推送给网页。

状态 `health.gateway` 应包含 `last_http_ms`、`max_http_ms`、
`http_requests`、`http_failures`、`ra8_status_age_ms`、`thermal_age_ms` 和
`pressure_age_ms`，用于直接判断瓶颈位于 RA8 UART、Wi-Fi 还是 HTTPS。

ESP32 在 `device/hello` 的 `metadata` 中上报 `local_ip`、`gateway_ip`、
`subnet_mask`、`mac`、`video_url` 和 `audio_url`。服务器分别显示设备自报的
局域网地址与反向代理观察到的地址，不能把代理地址误认为 ESP32 的热点 IP。
