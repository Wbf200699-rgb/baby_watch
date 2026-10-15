# P4 切换 Wi-Fi 并烧录

这些脚本会使用指定 Wi-Fi 重新编译 ESP32-P4X-EYE 网关固件，并可直接
烧录。Wi-Fi 是编译期配置，换热点后必须重新编译和烧录。

## 准备

1. 安装 ESP-IDF 5.5.x，并设置 `IDF_PATH`；当前项目验证版本为 5.5.4。
2. 至少运行一次项目根目录的 `tools/demo_configure_*`，把设备 ID 和设备
   密钥保存到仓库外的 `~/.baby_monitor_demo.json`。
3. 实际烧录时连接 P4 的 USB Serial/JTAG 接口。Windows 通常为 `COM3`，
   但应以设备管理器当前枚举结果为准。

脚本不会显示设备密钥。新 Wi-Fi 密码默认使用隐藏输入，也可在调用前临时
设置 `P4_WIFI_PASSWORD` 环境变量。不要把密码写进脚本或提交到 Git。

## Windows

在项目的 `server` 目录执行：

```powershell
# 只读预检
.\deploy\flash_p4_wifi.ps1 -WifiSsid "答辩热点" -CheckOnly

# 只生成固件，不访问硬件
.\deploy\flash_p4_wifi.ps1 -WifiSsid "答辩热点" -BuildOnly

# 生成并烧录，成功后把该 Wi-Fi 保存到仓库外配置
.\deploy\flash_p4_wifi.ps1 -WifiSsid "答辩热点" -Port COM3 -SaveWifi
```

## macOS / Linux

```bash
./deploy/flash_p4_wifi.sh --ssid "答辩热点" --check-only
./deploy/flash_p4_wifi.sh --ssid "答辩热点" --build-only
./deploy/flash_p4_wifi.sh --ssid "答辩热点" \
  --port /dev/cu.usbmodemXXXX --save-wifi
```

## 切换服务器

默认连接公网：

```text
https://renesas.zyxserver.online
```

连接电脑热点中的本地服务器时追加：

```powershell
-Server http://192.168.137.1:8088
```

或：

```bash
--server http://192.168.137.1:8088
```

脚本会自动把 `https://` 转为 `wss://`，把 `http://` 转为 `ws://`。

## 安全与恢复

- `-CheckOnly` / `--check-only` 不构建、不烧录，也不改外部配置。
- `-BuildOnly` / `--build-only` 不访问 P4 硬件。
- `-SaveWifi` / `--save-wifi` 仅在构建或烧录成功后写入用户目录配置。
- 生成目录 `build_p4_wifi` 可能包含展开后的编译期配置，已被 Git 忽略，
  不要压缩后分享或加入纯代码交付包。
- 要恢复当前公网固件，可重新使用原 Wi-Fi 运行本脚本，或使用项目根目录
  已有的 `tools/demo_restore_p4_cloud_*` 脚本。
