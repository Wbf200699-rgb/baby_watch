# 手机端通信协议 v1

本文定义手机应用与婴儿监护服务器之间的稳定接口。手机端不得直接使用
`/api/dashboard/*` 网页内部接口，也不得持有 ESP32 的设备密钥。

## 1. 基本约定

- 公网基址：`https://renesas.zyxserver.online`
- REST/SSE 前缀：`/api/mobile/v1`
- WebSocket 使用同一域名的 `wss://`
- JSON 编码：UTF-8
- 时间：ISO 8601 UTC，例如 `2026-07-24T10:15:30.123+00:00`
- 当前 `protocol_version`：`1`
- 生产环境只允许 HTTPS/WSS，不接受明文口令传输。

所有 REST、SSE 和 WebSocket 请求使用 HTTP Basic Auth：

```http
Authorization: Basic base64(username:password)
```

用户名和密码由部署人员提供。手机端应存入系统安全存储，不写入日志或源码。

错误响应统一为：

```json
{"ok":false,"error":"device not found"}
```

## 2. 能力发现

```http
GET /api/mobile/v1/capabilities
```

响应包含协议版本、可用矩阵、媒体格式、环境传感器字段和当前服务器接受的全部语义命令。
手机端启动后应读取一次，不要在客户端硬编码服务器可能尚未支持的按钮。BME688 能力位于
`environment_sensors.bme688`；其中 `path` 给出规范字段路径，
`gas_semantics=raw_resistance_only` 且 `bsec=false` 明确禁止把气阻显示成 VOC/IAQ。

## 3. 设备和实时状态

### 3.1 设备列表

```http
GET /api/mobile/v1/devices
```

```json
{
  "type":"mobile.devices",
  "protocol_version":1,
  "server_time":"2026-07-24T10:15:30.123+00:00",
  "devices":[{
    "id":"esp32cam-01",
    "name":"esp32cam-01",
    "online":true,
    "first_seen":"...",
    "last_seen":"...",
    "firmware":"...",
    "hardware":"ESP32-S3",
    "wifi_rssi":-48,
    "capabilities":["ra8","s3_video"]
  }]
}
```

### 3.2 当前快照

```http
GET /api/mobile/v1/devices/{device_id}/snapshot
```

关键字段：

- `device`：连接、固件、RSSI 和在线状态；`matrix_connected` 仅表示独立矩阵 WSS 状态，
  不参与设备在线判定。
- `telemetry`：最新 RA8/ESP32 遥测。其下有 `sensors`、`inference`、`health`。
- `matrices`：矩阵元数据，不含大数组；完整值从矩阵接口读取。
- `diagnostics`：服务器统一诊断结果，包含 `overall`、计数和 `items[].channels[]`，可直接显示哪个部件、哪个通道异常。
- `media.video/audio`：上传端是否可用、监听数和最近一帧元数据。
- `fusion`：已解码的并行上下文、全局 L0～L3 等级、主事件和全部活动事件。
- `vision_overlay`：映射到 S3 画面的归一化检测框；只用于预览，不参与 RA8 本地决策。
- `overlay_calibration`：RA8 串口摄像头到 S3 摄像头的当前四点映射参数。
- `links`：当前设备对应的 SSE、矩阵、音视频和命令路径。
- `links.stories` / `links.advisories`：AI 故事和遥测解释接口。AI 是否可用以
  `/capabilities` 的 `ai.enabled` 为准。

诊断状态只有 `ok`、`warning`、`offline`、`fault`。手机端颜色应按状态字段决定，不解析中文
`summary/detail` 文本。

#### BME688 字段

支持 BME688 的设备在最新快照及后续 SSE `snapshot` 中提供：

```json
{
  "telemetry": {
    "sensors": {
      "environment": {
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
}
```

换算方式为 `t_mc / 1000 = °C`、`rh_mp / 1000 = %RH`、
`p_pa / 100 = hPa`、`gas_ohm / 1000 = kΩ`。`flags` 的 bit0/bit1/bit2
依次为新样本、气阻有效、加热器稳定。手机端应分别展示有效状态：

- 温湿度和气压以 `s == 3`、值域有效且样本未过期为准。
- 原始气阻还必须满足 `(flags & 0x06) == 0x06`。
- `gas_ohm` 只能标为“原始气阻（Ω/kΩ）”。系统没有运行 BSEC，不得显示成 VOC、
  eCO₂、VSC 或 IAQ。

旧设备可能没有 `bme688`，客户端必须将其视为“不支持/无数据”，不能影响 SCD41
CO₂ 卡片。服务器输出使用上述规范嵌套路径；客户端只需在兼容旧服务器时额外读取
`telemetry.sensors.bme688`。

### 3.3 完整矩阵

```http
GET /api/mobile/v1/devices/{device_id}/matrices/thermal
GET /api/mobile/v1/devices/{device_id}/matrices/pressure
```

```json
{
  "type":"mobile.matrix",
  "protocol_version":1,
  "server_time":"...",
  "device_id":"esp32cam-01",
  "matrix":{
    "kind":"pressure",
    "sequence":123,
    "rows":16,
    "cols":16,
    "unit":"raw",
    "received_at":"...",
    "minimum":0,
    "maximum":1532,
    "fps":3.98,
    "frame_age_ms":126,
    "drops":0,
    "delayed":false,
    "values":[0,1,2]
  }
}
```

`values` 按行优先排列，长度必须等于 `rows * cols`。热成像通常为 `80 * 64`，单位
`deci_c`；压力通常为 `16 * 16`，单位 `raw`。App 只替换同类矩阵的最新帧，不按序播放
积压；`frame_age_ms > 2000` 或 `delayed=true` 时显示“数据延迟”。

## 4. 实时传感器 SSE

```http
GET /api/mobile/v1/devices/{device_id}/stream
Accept: text/event-stream
```

连接后第一条一定是 `snapshot`：

```text
event: snapshot
data: {"type":"mobile.snapshot",...}

```

后续事件：

| SSE event | `data.type` | 内容 |
|---|---|---|
| `snapshot` | `mobile.snapshot` | 新遥测或设备状态，替换当前快照 |
| `matrix` | `mobile.matrix` | 完整最新矩阵，按 `kind` 替换 |
| `event` | `mobile.event` | 新安全事件/警报 |
| `event_ack` | `mobile.event_ack` | 警报确认状态 |
| `command` | `mobile.command` | 命令状态变化 |
| `log` | `mobile.log` | 新设备日志，可按需显示 |
| `ai_story` | `mobile.ai_story` | 故事生成、合成、下发或失败状态变化 |
| `ai_advisory` | `mobile.ai_advisory` | 新的遥测综合提示 |
| `ai_advisory_ack` | `mobile.ai_advisory_ack` | 用户已读状态变化 |

服务器每 15 秒发送 SSE 注释心跳 `: keepalive`。连接断开后采用 1、2、4、8、15 秒上限的
指数退避重连；重连成功后用首条快照覆盖本地状态。v1 不保证断线期间事件重放，历史警报
应使用事件列表接口补齐。

### 4.1 事件列表与确认

```http
GET  /api/mobile/v1/devices/{device_id}/events?limit=50
POST /api/mobile/v1/devices/{device_id}/events/{event_id}/ack
```

## 5. 视频和声音预览

```text
wss://renesas.zyxserver.online/api/mobile/v1/devices/{device_id}/media/video/ws
wss://renesas.zyxserver.online/api/mobile/v1/devices/{device_id}/media/audio/ws
```

WebSocket 握手也必须带 Basic `Authorization` 头。订阅建立后，服务器自动通知 ESP32-S3
开始对应媒体上传；最后一个订阅者退出后自动停止上传。手机端不用另发“开启预览”命令。

每条二进制 WebSocket message 是一帧完整的 `S3MD/1` 数据，不能把多条 message 拼接，
也不能把一条拆分后单独解析。32 字节头采用网络字节序（big-endian）：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `S3MD` |
| 4 | 1 | version | `1` |
| 5 | 1 | media_type | `1`=JPEG，`2`=PCM 音频 |
| 6 | 2 | flags | bit0=`discontinuity` |
| 8 | 4 | sequence | 无符号帧序号 |
| 12 | 8 | timestamp_us | 设备单调时钟，微秒 |
| 20 | 4 | payload_length | 负载字节数 |
| 24 | 2 | meta0 | 见下表 |
| 26 | 2 | meta1 | 见下表 |
| 28 | 2 | meta2 | 见下表 |
| 30 | 2 | meta3 | 见下表 |

必须验证 `message.length == 32 + payload_length`。

视频：`meta0=width`、`meta1=height`、`meta2=JPEG quality`、`meta3=0`，负载是完整 JPEG
（以 `FF D8` 开始、`FF D9` 结束）。手机端可直接交给图片解码器，允许丢弃旧帧，只显示
最新帧。

音频：`meta0=16000`、`meta1=320 samples`、`meta2=1 channel`、`meta3=16 bits`，负载为
little-endian signed PCM16。每帧 20 ms、640 字节。若序号跳变或 `flags.bit0=1`，清空播放
缓冲；建议仅缓存 60-120 ms，过期帧直接丢弃。

### 5.1 在 S3 视频上叠加 RA8 检测框

视频二进制帧保持原样，服务器不会重新编码 JPEG。手机端从最新 snapshot/SSE 的
`vision_overlay` 读取框，并叠加在当前 S3 JPEG 上：

```json
{
  "vision_overlay": {
    "schema_version": 1,
    "enabled": true,
    "calibration_status": "calibrated",
    "source": "ra8_serial_camera",
    "target": "esp32s3_camera",
    "source_frame": {
      "sequence": 77,
      "frame_sequence": 51,
      "width": 320,
      "height": 240,
      "fusion_eligible": true,
      "age_ms": 180
    },
    "stale": false,
    "boxes": [{
      "class_id": 0,
      "label": "infant",
      "score_x1000": 812,
      "x": 0.12,
      "y": 0.18,
      "width": 0.51,
      "height": 0.62,
      "polygon": [[0.12,0.18],[0.63,0.18],[0.63,0.80],[0.12,0.80]]
    }],
    "thermal": {"enabled": false,"status": "reserved_until_heated_target_calibration"}
  }
}
```

`x/y/width/height` 和 `polygon` 都是目标 S3 画面的 `0..1` 归一化坐标。采用
`object-fit: contain` 时，必须先计算 JPEG 在控件中的实际可见矩形，再乘归一化坐标；不能
用整个控件尺寸，否则黑边会造成框偏移。`stale=true`、`enabled=false` 或视频未显示时必须
隐藏框。RA8 视觉与 S3 视频不是硬件同步帧，界面应显示 `source_frame.age_ms`，不宣称逐帧
同步。

四点映射在服务器网页标定并持久化，手机只读取结果。标定点应选择两台摄像头都能稳定
识别的四个床面固定点，并按相同顺序填写。热成像叠加字段已预留，但当前固定关闭。

客户端可每 20 秒发送文本心跳：

```json
{"type":"media.heartbeat","version":1,"sequence":1}
```

服务器回 `media.heartbeat_ack`。WebSocket 断开后采用指数退避重连。

## 6. 用户命令

### 6.1 下发

```http
POST /api/mobile/v1/devices/{device_id}/commands
Content-Type: application/json
```

```json
{
  "action":"audio.play_track",
  "args":{"track":2},
  "request_id":"phone-uuid-0001"
}
```

`request_id` 建议使用手机生成的 UUID。同一设备重复提交同一个 `request_id`，服务器返回原
命令且 `duplicate=true`，不会重复播放或重复开始录音。

响应：

```json
{
  "type":"mobile.command",
  "protocol_version":1,
  "duplicate":false,
  "command":{
    "id":42,
    "device_id":"esp32cam-01",
    "request_id":"phone-uuid-0001",
    "action":"audio.play_track",
    "status":"queued",
    "created_at":"...",
    "sent_at":null,
    "acked_at":null,
    "delivery_attempts":0,
    "result":null
  }
}
```

### 6.2 查询状态

```http
GET /api/mobile/v1/devices/{device_id}/commands/{command_id}
```

状态：

- `queued`：等待 ESP32 网关领取。
- `sent`：网关已经领取；超时可能重投。
- `acked`：网关接受并转发成功。
- `failed`：网关拒绝或无法映射。

普通 ESP32 回执 `forwarded to RA8` 只能证明命令已写入 RA8 串口，不能证明扬声器已经出声、
文件已经落盘。手机端应同时观察后续 `telemetry.sensors.audio_player`、`microphone` 或
`camera` 状态，超时后提示“命令已转发但设备状态未确认”。

暖光灯也采用同样的状态确认：命令 ACK 后继续等待
`telemetry.sensors.warm_light.on` 变为目标值。`s=1` 表示 GPIO 就绪，`s=2` 表示 GPIO
配置或写入失败；`pin=711`、`ah=1` 分别表示 P711 和高电平吸合。手机开关在设备离线或
`s!=1` 时必须禁用；下发后显示等待状态，最多等待能力字段 `confirmation_timeout_ms` 指定的
8 秒。超时后恢复为最后一次实际上报值，并提示“命令已转发但设备状态未确认”。

拍一拍电机使用与暖光灯相同的保持型继电器控制。App 下发
`soother.set` 和布尔参数 `on`，P4 分别映射为 RA8 文本命令 `soother_on` 或
`soother_off`。RA8 在 P914 持续输出目标电平，上电默认关闭。App 应等待
`telemetry.sensors.soother.on` 等于目标值，而不能只看网关 ACK。该命令是幂等的，
相同目标状态可以安全重投；当前没有电机侧物理反馈，状态表示 RA8 成功写入的 GPIO 状态。

AI 故事使用更强的回执语义：故事状态变为 `queued_for_playback` 时，P4 已下载并校验 WAV，
RA8 CPU1 已在 SD 卡上完成临时文件写入、整文件 CRC 校验和原子重命名，并已将文件加入播放
队列；真正的播放状态仍以 `telemetry.sensors.audio_player.story.s` 为准。

### 6.3 v1 动作表

| action | args | 作用 |
|---|---|---|
| `device.refresh` | 无 | 请求新 RA8 状态 |
| `audio.play_boot` | 无 | 播放一次 `BOOT.WAV` |
| `audio.play_track` | `track: 1..9999` | 播放指定曲目 |
| `audio.pause` / `audio.resume` / `audio.stop` | 无 | 播放控制 |
| `audio.next` / `audio.previous` | 无 | 切换曲目 |
| `audio.set_volume` | `percent: 0..100` | 设置音量 |
| `audio.refresh_library` | 无 | 重新扫描 SD 曲目 |
| `light.set` | `on: true/false` | 开关 P711 暖光灯继电器 |
| `soother.set` | `on: true/false` | 开关 P914 拍一拍电机继电器 |
| `microphone.start` / `microphone.stop` | 无 | RA8 麦克风采样控制 |
| `microphone.record.start` | 可选 `filename` | WAV 写入 SD |
| `microphone.record.stop` | 无 | 停止 WAV 录制 |
| `camera.capture` | 可选 `filename` | 串口摄像头拍一张 JPEG 到 SD |
| `camera.record.start` | 可选 `filename` | 开始串口摄像头采集 |
| `camera.record.stop` | 无 | 停止串口摄像头采集 |

`filename` 只能包含 1-8 个 ASCII 字母、数字、下划线或连字符，不带扩展名和路径。CPU1 保存时统一转为大写，该限制保证 RA8 只创建 FAT 8.3 短目录项。

App 可从 `GET /api/mobile/v1/capabilities` 的 `actuators.warm_light` 读取状态路径、
动作名、P711 引脚以及默认关灯策略，不应把这些硬编码为页面常量。
安抚器对应 `actuators.soother_pat`；它的 `mode=maintained`、`boot_default=off` 且
`physical_feedback=false`，UI 应显示保持型开关，并以遥测 `on` 字段确认状态。

能力对象同时提供 UI 语义：暖光灯与拍一拍电机均为 `control=switch`。
推荐动作位于移动快照的 `fusion.comfort`：

```json
{
  "available": true,
  "actions": ["light_on", "pat"],
  "reasons": ["cry"],
  "confidence": 88,
  "recommended_light": "on",
  "recommend_pat": true,
  "caregiver_check": false,
  "requires_confirmation": true,
  "blocked_reasons": []
}
```

App 可以在两个控件旁显示该建议，但不得自动执行。用户确认后才能发送命令。
暖光灯成功以 `warm_light.on` 达到目标值为准；拍一拍电机成功以
`soother.on` 达到目标值为准。`soother.set` 是幂等动作，网络超时后可以使用相同目标值重试。
当 `caregiver_check=true` 或 `blocked_reasons` 非空时，界面应突出显示人工检查建议，
而不是把拍一拍作为安全事件的替代处理。

## 7. AI 故事与婴儿状态提示

AI 功能是服务器的辅助解释层。RA8 本地融合、安全规则和硬件告警始终具有更高优先级；
手机端不得因为 AI 返回 `normal` 而隐藏本地告警。服务器也会强制阻止模型把本地
`attention/urgent` 证据降级。

自动遥测评测只在该设备至少有一条手机 SSE
`/api/mobile/v1/devices/{device_id}/stream` 在线时提交给大模型。手机断开后仍保存遥测、
执行 RA8 本地融合与安全告警，但不会产生自动 Qwen 请求；网页或手机主动发起的故事生成
和手动评测不受此门控影响。

### 7.1 生成并播放故事

```http
POST /api/mobile/v1/devices/{device_id}/stories
Content-Type: application/json
```

```json
{
  "theme":"月亮和小云朵",
  "child_age_months":18,
  "duration_seconds":60,
  "language":"zh-CN",
  "style":"calm",
  "autoplay":true,
  "request_id":"phone-story-uuid-0001"
}
```

`duration_seconds` 允许 30～240 秒，`style` 为 `calm`、`gentle` 或 `playful`。服务端会按时长生成足量正文并分句合成；长故事会在保持 PCM16LE 单声道的前提下自动使用 8 kHz，以满足 4 MiB 的端到端文件上限。
`request_id` 用于幂等：同一设备重复提交相同值只返回原任务，不会重复生成和播放。

响应和 SSE `ai_story` 的核心对象：

```json
{
  "type":"mobile.ai_story",
  "protocol_version":1,
  "duplicate":false,
  "story":{
    "story_id":"32位十六进制ID",
    "status":"queued",
    "title":"",
    "story_text":"",
    "audio_size_bytes":0,
    "audio_duration_ms":0,
    "command_id":null,
    "error":"",
    "created_at":"...",
    "updated_at":"..."
  }
}
```

状态顺序：

- `queued` → `generating` → `synthesizing`：服务器后台处理，不阻塞遥测。
- `ready`：WAV 已生成，但 `autoplay=false`，尚未下发。
- `queued_for_device`：命令等待 P4 领取或正在传输。
- `queued_for_playback`：RA8 已完整落盘并加入播放队列。
- `failed`：生成、下载、校验、SD 写入或命令执行失败；查看 `error`。

查询及重播：

```http
GET  /api/mobile/v1/devices/{device_id}/stories?limit=20
GET  /api/mobile/v1/devices/{device_id}/stories/{story_id}
POST /api/mobile/v1/devices/{device_id}/stories/{story_id}/replay
```

服务器只接受 8 kHz 或 16 kHz、单声道、PCM16LE WAV。P4 和 RA8 会再次检查长度、SHA-256、分块
CRC32 和整文件 CRC32；失败时不会留下可播放的半文件。

### 7.2 遥测综合提示

```http
GET /api/mobile/v1/devices/{device_id}/advisories?limit=50
POST /api/mobile/v1/devices/{device_id}/advisories/{advisory_id}/ack
```

新提示同时通过 SSE `ai_advisory` 推送：

```json
{
  "type":"mobile.ai_advisory",
  "protocol_version":1,
  "device_id":"esp32cam-01",
  "data":{
    "advisory_id":"...",
    "level":"attention",
    "title":"请查看婴儿状态",
    "summary":"...",
    "evidence":["RA8 本地融合或安全层要求关注"],
    "actions":["查看实时画面"],
    "confidence":0.82,
    "requires_user_attention":true,
    "source_sequence":1234,
    "acknowledged":false,
    "created_at":"..."
  }
}
```

`level` 只有 `normal`、`attention`、`urgent`。这是非医疗建议；AI 不可替代照护者检查，
也不能关闭、确认或降低 RA8 的本地告警。API 不可用或模型超时时，传感器、告警、音视频和
命令链路仍应正常运行。

## 8. 手机端推荐启动流程

1. 从安全存储读取服务器地址和 Basic 凭据。
2. `GET /capabilities`，检查 `protocol_version == 1`。
3. `GET /devices`，选择设备并显示在线状态。
4. `GET /snapshot` 后连接 SSE `/stream`。
5. 用户进入视频/音频预览页时才连接对应 WSS，离开页面立即关闭。
6. 下发命令时生成唯一 `request_id`；先显示 `queued`，再根据 SSE 或查询接口更新状态。
7. 若 `/capabilities.ai.enabled=true`，订阅 `ai_story` 和 `ai_advisory`；否则隐藏 AI 入口。
8. APP 回到前台或 SSE 重连后重新获取快照，并补查 stories/advisories，不依赖旧内存状态。

## 9. 兼容性规则

- v1 内新增 JSON 字段属于向后兼容，客户端必须忽略未知字段。
- 现有字段不会改变类型或语义；不兼容修改将使用 `/api/mobile/v2`。
- 媒体头版本独立于 REST 版本；未知 `S3MD` 版本必须拒绝，不能猜测解析。
- 手机端不得调用 `/api/v1/*` 设备上传接口，也不得调用任意 `ra8.command` 原始载荷。
