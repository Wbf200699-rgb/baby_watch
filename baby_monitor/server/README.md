# 婴儿监护数据服务器

本服务用于 ESP32/RA8 状态监控、警报展示、SD 训练数据采集控制和模型版本管理。大体量原始数据不通过网络上传。

## 启动

```powershell
cd server
python -m venv .venv
.\.venv\Scripts\pip install -r requirements.txt
$env:BABY_MONITOR_DEVICE_KEYS="demo-device:dev-secret"
.\.venv\Scripts\python -m uvicorn app.main:app --host 0.0.0.0 --port 8088
```

浏览器打开 `http://127.0.0.1:8088`。局域网 ESP32 使用电脑的局域网 IP 和端口 `8088`。

实时页面优先通过 `WSS /api/dashboard/realtime/ws` 接收遥测和最新矩阵；浏览器按类别
只保留最后一帧并在下一次绘制周期刷新，避免代理缓冲后集中释放旧帧。SSE
`/api/dashboard/stream` 仅作为旧部署和 WebSocket 故障时的兼容回退。新版
ESP32-P4X-EYE 将遥测/命令、压力与热成像矩阵、视频和音频拆为四条独立长连接；矩阵使用
`WSS /api/v1/device/matrices`。服务器在校验并更新内存最新帧后立即 ACK，SQLite 和浏览器
广播走容量为 2 的后台队列，网络忙时丢弃旧矩阵而不阻塞遥测。旧 HTTPS 批次和复用 WSS
仍保留一个兼容周期。页面的 FPS、帧龄、丢帧和“数据延迟”可用于判断瓶颈是在 RA8
串口、ESP32 网络还是服务器。遥测同样采用最新帧缓存和两条容量为 2 的后台队列，4 Hz
实时 ACK，普通 SQLite 历史降采样到 1 Hz。

压力矩阵页面默认显示阈值为 0，不丢弃任何正原始计数；低压增强只扩展低端色阶，峰值保持
用于观察短时轻触。“空床校准”会采集 20 帧并保存逐点中值基线到当前浏览器。该处理只影响
显示，服务器缓存、SD 数据和模型输入仍保留原始矩阵。热成像叠加只绘制当前婴儿/头部检测
框内的连续高温区域；视觉或热证据任一缺失时不叠加整幅热图。热图和趋势图均可用鼠标查看
对应点位的原始数值。

环境遥测在原有 `sensors.environment` SCD41 CO₂/温湿度字段下增加可选
`bme688` 对象。服务端会保存并实时展示 BME688 温度、湿度、气压和原始气阻，也兼容接收
早期网关的 `sensors.bme688` 并在入库前归一。气阻字段没有经过 BSEC，只能解释为 Ω/kΩ
原始电阻，不能作为 VOC 浓度或 IAQ。

网页或手机端建立音视频 WebSocket 后，服务器会按订阅者数量自动通知 ESP32-S3 开始
上传；服务器只在内存中保留最新 JPEG，并把 PCM 音频转发给当前监听端。最后一个预览
连接关闭后自动停止媒体上传，传感器遥测继续运行。

运行时遥测、日志和未被训练会话引用的矩阵会按设备自动滚动清理，默认分别保留
20000、10000 和 2000 行。可通过 `BABY_MONITOR_TELEMETRY_RETENTION_ROWS`、
`BABY_MONITOR_LOG_RETENTION_ROWS`、`BABY_MONITOR_MATRIX_RETENTION_ROWS` 调整；
训练会话引用的矩阵不会被清理。

## 采集流程

1. 在“训练采集”页先选择训练任务，再填写该任务的规范标签；同一轮只改变一个主要条件。
2. 记录数据集划分、假人姿态、床区、毛巾覆盖、加热器、配重和环境声音等实验条件。划分应按完整实验轮次或日期分组，不能把同一连续片段拆到训练集和测试集。
3. 空床时先点击“空床压力校准”，确认矩阵稳定后再放置假人；页面可按当前条件建议标签，但正式采集前仍需人工确认。
4. 串口摄像头、麦克风音频、床温、热成像和压力矩阵都有独立开始/停止按钮。只启动当前任务需要的数据，避免无关数据混入会话。
5. 麦克风和摄像头原文件写入 RA8 SD 卡；停止后使用页面的 SD 导入功能关联原文件。温度、热成像和压力由实时流直接归档。
6. 停止采集后检查样本数、文件状态和备注，在“样本审核”中剔除模糊、遮挡错误、传感器过渡帧和标签不一致样本，再导出训练数据。
7. 将训练后的模型与 manifest 放到 `BABY_MODELS/<model_id>`。

完整标签体系、实验摆放、推荐时长、重复次数和验收方法见
[`docs/MULTIMODAL_WARNING_AND_COLLECTION_GUIDE_zh-CN.md`](../docs/MULTIMODAL_WARNING_AND_COLLECTION_GUIDE_zh-CN.md)。

麦克风卡片会显示 RA8 的 SD 写入器状态。设备返回“命令已转发”不等于文件已经落盘；
应看到“SD 文件已打开”或“SD 写入中”后再进行正式采集。

设备上传协议见 [ESP32_SERVER_PROTOCOL.md](ESP32_SERVER_PROTOCOL.md)，手机端 REST/SSE/WSS
协议见 [MOBILE_APP_PROTOCOL.md](MOBILE_APP_PROTOCOL.md)。

## 大模型故事与状态解释

大模型功能默认关闭，且只运行在服务器。它不会参与 RA8 的硬实时安全决策：

- 手机或网页提交主题后，Qwen 生成故事并合成 16 kHz 单声道 PCM16 WAV；网页“AI 助手”
  页面还能手动触发一次遥测综合评估并查看证据、建议与历史提示。
- P4 在独立后台任务下载和校验文件，经现有 921600 UART 分块发送给 RA8。
- CPU1 在 SD/FAT 锁内写临时文件，整文件 CRC 正确后原子改名并交给现有扬声器播放。
- 服务器按周期或安全事件触发遥测解释，并通过手机 SSE 推送提示；模型不能降低本地融合层
  已给出的风险等级。

在部署机的 `server/.env` 中设置下列变量，真实 Key 不得写入源码、Compose 或 Git：

```text
BABY_MONITOR_AI_ENABLED=1
DASHSCOPE_API_KEY=<new-secret-key>
BABY_MONITOR_QWEN_MODEL=qwen-plus
BABY_MONITOR_QWEN_TTS_MODEL=qwen-audio-3.0-tts-flash
BABY_MONITOR_QWEN_TTS_VOICE=longanhuan_v3.6
```

如使用阿里云业务空间专属域名，再设置 `BABY_MONITOR_QWEN_CHAT_URL` 和
`BABY_MONITOR_QWEN_TTS_URL`。启用后用 `GET /health` 检查 `ai.enabled=true`；该响应不会
返回 Key。完整边界、提示词和验收规则见
[AI_STORY_ADVISORY_ARCHITECTURE_zh-CN.md](../docs/AI_STORY_ADVISORY_ARCHITECTURE_zh-CN.md)。

任何已经出现在聊天、截图或日志中的 Key 都应先吊销并重新生成，再写入服务器 `.env`。

## Docker 公网部署

```bash
git clone https://github.com/suchen2333/baby_monitor.git
cd baby_monitor/server
cp deploy/.env.example .env
docker run --rm caddy:2.10-alpine caddy hash-password --plaintext 'replace-this-password'
# 将输出及设备密钥填入 .env
docker compose up -d --build
```

生产配置由 Caddy 自动申请 HTTPS 证书。网页使用 HTTP Basic Auth；`/api/v1/*` 由设备 ID 和设备密钥鉴权。SQLite、模型和日志保存在 Docker 命名卷中，更新容器不会丢失。

当前 `renesas.zyxserver.online` 指向云主机时，树莓派应运行：

```bash
sh deploy/start_pi.sh
```

树莓派上不要直接运行 `docker compose up`：基础 Compose 不发布应用端口，漏掉
`compose.pi.yaml` 会让云主机访问 `100.81.192.84:8088` 时得到 502。应始终使用
`start_pi.sh`、`install_or_update.sh`，或显式同时传入
`-f compose.yaml -f compose.pi.yaml`。

然后将 `deploy/nginx-renesas.conf` 安装到云主机 Nginx。云主机通过 Tailscale `100.81.192.84:8088` 访问树莓派。树莓派容器默认绑定本机所有接口，避免重启时 Docker 早于 `tailscale0` 启动而永久丢失端口映射；不要在路由器上转发 8088，公网入口仍只保留云主机 Nginx。创建网页密码并申请证书：

```bash
sudo htpasswd -c /etc/nginx/.htpasswd-renesas admin
sudo certbot --nginx -d renesas.zyxserver.online
sudo nginx -t && sudo systemctl reload nginx
```

后续更新：

```bash
sh server/deploy/install_or_update.sh
```

视频控制默认保持 8 FPS，可在 `.env` 中设置 `BABY_MONITOR_VIDEO_TARGET_FPS=8..12`。在
ESP32-C6 媒体协处理器完成并发验证前建议保持 8；调整后重建应用容器即可生效。

P4X-EYE 四链路 30 分钟验收可使用：

```powershell
$env:BABY_MONITOR_ADMIN_USER = "..."
$env:BABY_MONITOR_ADMIN_PASSWORD = "..."
python server/tools/p4x_realtime_soak.py --server https://renesas.zyxserver.online --duration 1800 --strict --output p4x_soak.json
```

探针会同时订阅原始 S3MD 视频/音频并轮询遥测、矩阵、ACK、队列和重连指标；密码不会写入结果文件。

热成像共享缓冲和过渡帧门控可单独运行 30 分钟验收。该探针允许当前验证阶段的
2 Hz 热成像发布频率，并检查完整帧、CRC、seqlock、质量字段及错误计数是否稳定：

```powershell
$env:BABY_MONITOR_ADMIN_USER = "..."
$env:BABY_MONITOR_ADMIN_PASSWORD = "..."
python server/tools/thermal_quality_soak.py --server https://renesas.zyxserver.online --duration 1800 --output thermal_quality_soak.json
```

树莓派首次部署或更新恢复机制后，安装一分钟自检定时器：

```bash
cd ~/baby_monitor/server
sudo install -m 0644 deploy/baby-monitor-recover.service /etc/systemd/system/
sudo install -m 0644 deploy/baby-monitor-recover.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now baby-monitor-recover.timer
```

该定时器会检查本机 `/health` 和 Docker 健康状态；连续两次失败时，使用完整的 Pi Compose 配置重建应用容器。

## 本地热点备用

电脑安装 Docker Desktop 后，在 `server` 目录运行：

```powershell
.\deploy\start_local.ps1 -Port 8088 -DeviceKeys "esp32cam-01:设备密钥"
```

ESP32 与电脑连接同一热点后，将服务器地址改为电脑热点网卡 IP，例如 `http://192.168.137.1:8088`。本地模式不依赖域名、树莓派或互联网。

电脑没有 Docker 时可直接运行：

```powershell
.\deploy\start_local_python.ps1 -Port 8088 -DeviceKeys "esp32cam-01:设备密钥"
```

私有 GitHub 仓库无法让树莓派匿名拉取时，从本机更新树莓派：

```powershell
.\deploy\publish_to_pi.ps1
```

建议先执行只读预检，再进行一键发布：

```powershell
.\deploy\publish_to_pi.ps1 -CheckOnly
.\deploy\publish_to_pi.ps1
```

macOS/Linux 使用等价脚本：

```bash
./deploy/publish_to_pi.sh --check-only
./deploy/publish_to_pi.sh
```

发布脚本会校验归档 SHA-256，保留树莓派现有 `.env` 和 Docker 数据卷，
等待容器健康后再检查树莓派本机与公网 `/health`。任何一步失败都会以非零状态退出。

该脚本保留树莓派上的 `.env` 和数据卷，只替换应用文件并重建容器。

### 为 P4 切换 Wi-Fi 并烧录

P4 的 Wi-Fi、设备鉴权和服务器地址属于编译期配置。新的脚本位于同一个
`deploy` 目录，支持源码仓库和 `RA8_P4_SERVER_RUNTIME` 纯代码目录。
设备 ID/密钥默认从仓库外的 `~/.baby_monitor_demo.json` 读取；Wi-Fi 密码
使用隐藏输入，或从临时环境变量 `P4_WIFI_PASSWORD` 读取，不应写入 Git。

Windows PowerShell：

```powershell
# 只检查 ESP-IDF、源码和外部设备配置，不构建也不烧录
.\deploy\flash_p4_wifi.ps1 -WifiSsid "答辩热点" -CheckOnly

# 只构建新 Wi-Fi 固件，用于先验证编译
.\deploy\flash_p4_wifi.ps1 -WifiSsid "答辩热点" -BuildOnly

# 编译并烧录；未指定 Port 时由 ESP-IDF 自动选择设备
.\deploy\flash_p4_wifi.ps1 -WifiSsid "答辩热点" -Port COM3 -SaveWifi
```

macOS/Linux：

```bash
./deploy/flash_p4_wifi.sh --ssid "答辩热点" --check-only
./deploy/flash_p4_wifi.sh --ssid "答辩热点" --build-only
./deploy/flash_p4_wifi.sh --ssid "答辩热点" --port /dev/cu.usbmodemXXXX --save-wifi
```

默认仍连接公网 `https://renesas.zyxserver.online`。如需连接电脑本地服务器，
追加 `-Server http://192.168.x.x:8088` 或
`--server http://192.168.x.x:8088`。脚本会自动生成对应的 `ws://` 或
`wss://` 地址。`-SaveWifi`/`--save-wifi` 只在构建或烧录成功后更新用户目录
中的外部配置，不会修改仓库文件。
