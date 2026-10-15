# 答辩现场本地服务器与 P4 切换

本方案是手动双端点，不做自动故障切换。切换时必须让 P4 和手机同时指向同一台服务器。
设备、Wi-Fi 和网页凭据保存在用户主目录的 `.baby_monitor_demo.json`，不会写入仓库或固件源码。

## 首次配置

Windows PowerShell：

```powershell
.\tools\demo_configure_windows.ps1
```

macOS：

```bash
./tools/demo_configure_macos.sh
```

填写 P4 使用的 Wi-Fi、设备 ID/Key 和答辩电脑的局域网 IPv4。电脑和 P4 必须连接同一个热点或路由器。
Windows 脚本可以从本机已有 P4 构建中导入凭据，但不会显示凭据内容。macOS 首次使用时手动填写。

只检查环境、不构建也不烧录：

```powershell
.\tools\demo_flash_p4_local_windows.ps1 -CheckOnly
.\tools\demo_restore_p4_cloud_windows.ps1 -CheckOnly
```

macOS 对应在命令末尾添加 `--check-only`。

## 切到电脑本地服务器

1. 启动服务器并保持窗口打开：

   Windows：`.\tools\demo_server_windows.ps1`

   macOS：`./tools/demo_server_macos.sh`

2. 将 P4 的 USB **Debug** 口连接电脑，然后烧本地地址固件：

   Windows：`.\tools\demo_flash_p4_local_windows.ps1`

   macOS：`./tools/demo_flash_p4_local_macos.sh`

   自动识别失败时传端口，例如 Windows 加 `-Port COM24`，macOS 加 `--port /dev/cu.usbmodemXXXX`。

3. 手机 App 在服务器设置中手动选择“电脑”，填写脚本输出的 `http://局域网IP:8088`。

本地数据保存在 `server/data/demo_fallback/`，与公网树莓派数据库分离。默认关闭 Qwen，硬件告警、预览、控制和数据采集仍可工作。

## 恢复当前公网服务

P4 接在 Debug 口后执行：

Windows：

```powershell
.\tools\demo_restore_p4_cloud_windows.ps1
```

macOS：

```bash
./tools/demo_restore_p4_cloud_macos.sh
```

然后在手机 App 中手动切回 `https://renesas.zyxserver.online`。公网恢复脚本重新构建当前源码，地址固定为正式 HTTPS/WSS，不依赖本地服务器。

## 现场检查

- P4 Debug 口应识别为 Espressif USB Serial/JTAG；SEGGER/J-Link 串口不是 P4 ROM 下载端口。
- 本地网页 `/health` 应返回 200，P4 遥测、矩阵、视频、音频连接互不替换。
- Windows 防火墙首次弹窗时允许 Python 在专用网络通信。
- 切换热点后若电脑 IPv4 改变，重新运行首次配置并再次烧本地 P4 固件。
- 不要把 `.baby_monitor_demo.json`、设备 Key、Wi-Fi 密码或大模型 Key 发给他人或提交 Git。
- 手机 App 的“云端/Plan B”切换只改变手机连接；P4 必须用本页对应脚本切到同一端点。
