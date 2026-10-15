# 大模型故事与遥测解释层设计

## 1. 目标和边界

本功能增加两个互相独立的后台任务：

1. 手机提交主题，服务器调用 Qwen 生成安全的中文故事并合成 WAV，P4 下载后传给 RA8，
   CPU1 写入 SD 卡并由现有扬声器播放。
2. 服务器按周期或本地安全事件触发 Qwen，对最新的结构化遥测做简短解释，并通过手机 SSE
   推送 normal/attention/urgent 提示。

RA8 本地模型、融合规则和安全告警始终是权威决策层。Qwen 是解释层，不参与硬实时控制，
不允许关闭或降低本地告警；云 API 故障不能阻塞遥测、矩阵、媒体、命令或本地报警。

## 2. 数据流

```text
手机 POST /stories
  -> Server bounded story queue
  -> Qwen chat JSON
  -> Qwen non-streaming TTS WAV
  -> Server atomic WAV storage + SHA256/CRC32
  -> existing command WSS: story.audio.play
  -> P4 background HTTPS download and validation
  -> existing RA8 UART: BEGIN / CHUNK / END
  -> CPU0 validation and CPU0/CPU1 mailbox
  -> CPU1 FAT lock, .TMP write, CRC, atomic rename
  -> existing WAV player
  -> RA8 telemetry aud.story
  -> Server/mobile SSE
```

```text
RA8 telemetry/safety event
  -> Server latest-only compact snapshot
  -> bounded assessment queue
  -> Qwen JSON advisory
  -> server database + SSE ai_advisory
  -> phone notification view
```

故事队列容量为 4，遥测评估队列容量为 8。遥测只保留每台设备的最新快照；队列拥塞时
丢弃旧评估请求，不形成无界积压。

## 3. 提示词约束

故事系统提示要求：

- 只输出 JSON，字段为 title、story_text、age_range、estimated_seconds、safety_notes。
- 内容温和、舒缓、无惊吓、无医疗结论、无危险动作模仿。
- 必须有明确主人公、具体目标、温和阻碍、至少两次主动行动、自然转折和完整结局，事件之间有因果关系。
- 环境描写不超过正文 20%，不得用月光、星星、云朵、微风等意象堆砌代替情节。
- 不要求婴儿回应，正文长度按 30～240 秒目标时长计算，结尾安静自然。
- 用户只控制主题、0～72 月龄、目标时长和有限风格枚举。

遥测解释系统提示要求：

- 明确不是医疗诊断，只能解释给定字段，不能臆测缺失传感器。
- 只输出 JSON：level、title、summary、evidence、actions、confidence、
  requires_user_attention。
- urgent 只能建立在已有明确风险证据上。
- 代码在模型输出后再次执行最低风险级别门控：RA8 本地 level/severity 非零时，模型不能
  输出更低等级。

发送给模型的是压缩后的状态摘要，不含完整 80x64/16x16 矩阵、音频或视频，避免带宽、
隐私和 token 浪费。

## 4. 音频格式和完整性

- 服务器只接受 PCM16LE、单声道、16000 Hz、无压缩 WAV。
- 最大文件 4 MiB；P4 分块 448 字节。
- 服务器保存时使用 `.TMP` 后原子替换。
- P4 校验 HTTP 长度、SHA-256、整文件 CRC32 和 RIFF/WAVE/fmt/data 结构。
- RA8 校验每块 CRC32、严格 offset、最终长度和最终 CRC32。
- CPU1 在统一 SD/FAT 互斥锁内写 `/sd0/STORY/AIxxxxxx.TMP`，成功后原子改名为
  `/sd0/STORY/AIxxxxxx.WAV`；错误时删除临时文件。`/sd0/MUSIC` 只参与音乐曲目扫描，
  旧卡中遗留的 `AIxxxxxx.WAV` 也会被扫描器排除。
- BEGIN 和 CHUNK 支持相同 transfer_id 的幂等重试；P4 同一操作超时后最多重试一次。

## 5. UART 和接线

第一版复用现有 RA8-P4 全双工串口，故事传输在 P4 独立低优先级任务中进行，因此不需要
新增 UART：

- RA8 P603 TX -> P4 GPIO8 RX
- RA8 P602 RX <- P4 GPIO7 TX
- GND 共地
- 3.3 V TTL，921600 baud，8N1

故事传输期间状态帧可能有轻微延迟，但矩阵和公网媒体不经过这条文件队列。只有实测证明
现有 UART 对本地状态时延造成不可接受影响时，才考虑增加第二串口；当前代码不要求用户
增加接线。

详细二进制布局见 `server/ESP32_SERVER_PROTOCOL.md`。

## 6. 服务端配置

生产环境在 `server/.env` 设置：

```text
BABY_MONITOR_AI_ENABLED=1
DASHSCOPE_API_KEY=<从秘密管理器或手工写入，不提交 Git>
BABY_MONITOR_QWEN_MODEL=qwen-plus
BABY_MONITOR_QWEN_TTS_MODEL=qwen-audio-3.0-tts-flash
BABY_MONITOR_QWEN_TTS_VOICE=longanhuan_v3.6
BABY_MONITOR_AI_ASSESSMENT_INTERVAL_SECONDS=600
BABY_MONITOR_AI_EVENT_COOLDOWN_SECONDS=60
```

Qwen-Audio-TTS 使用北京地域。默认旧域名仍可用；若使用业务空间专属域名，在 `.env` 设置
`BABY_MONITOR_QWEN_TTS_URL` 和 `BABY_MONITOR_QWEN_CHAT_URL`，源码不保存 Workspace ID
或 API Key。

API Key 曾在聊天或日志中明文出现时，应在阿里云控制台立即吊销并创建新 Key。部署前可
通过 `/health` 的 `ai.enabled` 验证是否启用，但该接口绝不返回 Key。

## 7. 验收

1. AI 关闭或 API 超时时，遥测 4 Hz、矩阵、音视频和本地告警不受影响。
2. 同一个 `request_id` 重复创建故事只产生一个任务和一个文件。
3. 非 16 kHz/单声道/PCM16 WAV 在服务器阶段拒绝。
4. 篡改 HTTP 文件、任意分块或最终 CRC 后，RA8 不生成最终 WAV。
5. P4 复位后服务器按同一命令重投，RA8 不重复追加已确认分块。
6. 手机看到 `queued_for_playback` 时，RA8 SD 已存在完整文件；播放过程由
   `sensors.audio_player.story.s=3`，结束由 `s=4` 确认。
7. 模型返回 normal 但 RA8 本地 level=3 时，最终提示必须为 urgent。
8. 连续运行 30 分钟，AI 队列、内存和数据库任务不持续增长。
