# RA8 双核与 ESP32-S3 源代码及程序清单

> 生成时间：`2026-07-28T14:58:59+08:00`  
> Git 基线：`agent/ra8-camera-training-workflow` / `f26f0c2`  
> 生成工具：`tools/generate_ra8_s3_inventory.py`

## 1. 范围

本清单只收录 RA8P1 双核固件、ESP32-S3 网关、RA8-S3 协议工具以及 RA8 构建/烧录工具。
不收录服务端代码、网页、旧 ESP32-CAM、音频/视觉训练代码、数据集、训练产物、虚拟环境和运行数据。

RA8 固件编译所必需的已嵌入模型 C/H 和 NPU 编译输出仍然收录，并明确标记为
`embedded-model-generated`；它们是固件构建输入，不是训练代码或训练数据。

- 源码/配置文件：**673**
- 总行数：**1,153,803**（含 FSP/RTOS 和嵌入模型生成代码）
- 项目自编代码/配置：**29,237** 行（不含协议文档）
- Git 状态：modified 124；tracked-clean 466；untracked 83

完整逐文件清单：[`ra8_s3_source_inventory.csv`](ra8_s3_source_inventory.csv)  
程序级清单：[`ra8_s3_program_inventory.csv`](ra8_s3_program_inventory.csv)

## 2. 程序清单

| ID | 程序 | 目标 | 入口 | 用途 | 状态 |
| --- | --- | --- | --- | --- | --- |
| ra8_cpu0_m85 | RA8P1 CPU0 主控固件 | R7KA8P1 Cortex-M85 + Ethos-U55 | Multicore_top_CPU0/ra_gen/main.c; Multicore_top_CPU0/src/*_thread_entry.c | 视觉和哭声推理、多模态融合、分级告警、S3 UART、串口摄像头和双核控制 | 生产主固件 |
| ra8_cpu1_m33 | RA8P1 CPU1 外设固件 | R7KA8P1 Cortex-M33 | Multicore_top_CPU1/ra_gen/main.c; Multicore_top_CPU1/src/*_thread_entry.c | 热成像、压力矩阵、TMP117、SD/FAT、系统日志、录音写盘和扬声器播放 | 生产主固件 |
| esp32s3_gateway | ESP32-S3 网络与媒体网关 | ESP32-S3 CAM | firmware/esp32s3_gateway/main/gateway_main.c | RA8 UART 网关、联网遥测、命令转发和按需 JPEG/PCM 预览 | 当前 S3 网关 |
| ra8_s3_protocol_tools | RA8-S3 串口协议与可视化工具 | Windows/Linux PC + CH340 | tools/ra8_esp32_protocol/ra8_protocol.py; tools/ra8_esp32_protocol/ra8_live_monitor.py | 模拟 S3 读取 RA8、发命令并验证传感器和协议 | 硬件调试工具 |
| ra8_flash_tools | RA8 双核与视觉模型烧录工具 | Windows + J-Link | tools/program_ra8_internal.ps1; tools/program_vision_models.ps1 | 避免组合 SREC 的 0x80000000 超范围，并分别烧录 CPU 固件和视觉模型 | 构建/烧录工具 |

## 3. 源码规模

### 3.1 按组件

| 组件 | 文件数 |
| --- | --- |
| RA8 CPU0/M85 | 343 |
| RA8 CPU1/M33 | 284 |
| ESP32-S3 gateway | 16 |
| RA8-S3 protocol tools | 12 |
| RA8 build/test tools | 10 |
| RA8 shared common | 6 |
| RA8 solution | 1 |
| RA8-S3 root | 1 |

### 3.2 按类别

| 类别 | 文件数 |
| --- | --- |
| generated/vendor-source | 456 |
| application-source | 106 |
| embedded-model | 39 |
| protocol/IPC | 26 |
| tool/script | 19 |
| configuration/build | 15 |
| documentation/protocol | 11 |
| test | 1 |

### 3.3 按来源

| 来源 | 文件数 |
| --- | --- |
| vendor/FSP/RTOS | 390 |
| project-authored | 135 |
| FSP-generated | 70 |
| embedded-model-generated | 39 |
| FSP-generated-config | 35 |
| third-party | 3 |
| tool-generated | 1 |

## 4. CPU0 / Cortex-M85 线程

| 线程/入口 | 文件 | 职责 |
| --- | --- | --- |
| audio_infer | `Multicore_top_CPU0/src/audio_infer_thread_entry.c` | 哭声一级/二级推理与时序策略 |
| camera | `Multicore_top_CPU0/src/camera_thread_entry.c` | 串口摄像头探测、JPEG 分块和诊断 |
| esp32_uart | `Multicore_top_CPU0/src/esp32_uart_thread_entry.c` | RA8-S3 帧收发、冷启动握手和可靠事件 |
| fusion | `Multicore_top_CPU0/src/fusion_thread_entry.c` | 多模态证据、事件状态机和分级告警 |
| vision_preprocess | `Multicore_top_CPU0/src/vision_preprocess_thread_entry.c` | JPEG 解码、ROI 和视觉预处理 |
| vision_infer | `Multicore_top_CPU0/src/vision_infer_thread_entry.c` | 三视觉模型/NPU 调度与结果发布 |
| cpu1_start | `Multicore_top_CPU0/src/cpu1_start_entry.c` | CPU1 启动和双核就绪协调 |
| command_dispatch | `Multicore_top_CPU0/src/command_dispatch_thread_entry.c` | 命令分发扩展入口 |
| cloud_protocol | `Multicore_top_CPU0/src/cloud_protocol_thread_entry.c` | 云协议扩展入口 |
| file_request | `Multicore_top_CPU0/src/file_request_thread_entry.c` | 跨核文件请求扩展入口 |
| playback_control | `Multicore_top_CPU0/src/playback_control_thread_entry.c` | 播放控制扩展入口 |
| telemetry_upload | `Multicore_top_CPU0/src/telemetry_upload_thread_entry.c` | 遥测上传扩展入口 |
| m85_watchdog | `Multicore_top_CPU0/src/m85_watchdog_thread_entry.c` | M85 看门狗和健康监测 |

CPU0 的应用驱动位于 `Multicore_top_CPU0/src/thread_code/`，视觉模型运行输入位于
`Multicore_top_CPU0/src/vision_models/`。FSP 线程、引脚、时钟和中断由 `configuration.xml` 生成。

## 5. CPU1 / Cortex-M33 线程

| 线程/入口 | 文件 | 职责 |
| --- | --- | --- |
| thermal | `Multicore_top_CPU1/src/thermal_thread_entry.c` | HTPA 80x64 UART 整帧接收与发布 |
| pressure | `Multicore_top_CPU1/src/pressure_thread_entry.c` | M1616 16x16 压力矩阵 UART |
| temp | `Multicore_top_CPU1/src/temp_thread_entry.c` | PCA9548A + 5 路 TMP117 温度 |
| sd_card | `Multicore_top_CPU1/src/sd_card_thread_entry.c` | SDHI 初始化、恢复和文件系统就绪 |
| file_system | `Multicore_top_CPU1/src/file_system_thread_entry.c` | 共享 FAT 写入、日志、录音和图片落盘 |
| sd_audio_player | `Multicore_top_CPU1/src/sd_audio_player_thread_entry.c` | SD WAV 文件播放控制 |
| audio_capture | `Multicore_top_CPU1/src/audio_capture_thread_entry.c` | 音频采集兼容/扩展入口 |
| audio_feature | `Multicore_top_CPU1/src/audio_feature_thread_entry.c` | 音频特征兼容/扩展入口 |
| speaker_output | `Multicore_top_CPU1/src/speaker_output_thread_entry.c` | SSI 扬声器输出兼容/扩展入口 |
| m33_ipc_rx | `Multicore_top_CPU1/src/m33_ipc_rx_thread_entry.c` | 跨核 IPC 接收扩展入口 |
| m33_ipc_tx | `Multicore_top_CPU1/src/m33_ipc_tx_thread_entry.c` | 跨核 IPC 发送扩展入口 |
| file_update | `Multicore_top_CPU1/src/file_update_thread_entry.c` | 文件更新扩展入口 |
| m33_watchdog | `Multicore_top_CPU1/src/m33_watchdog_thread_entry.c` | M33 看门狗和健康监测 |

CPU1 的设备驱动位于 `Multicore_top_CPU1/src/thread_code/`。SD/FAT 是日志、麦克风 WAV、
串口摄像头 JPEG 和播放文件的共同资源，写入必须使用现有共享锁和就绪状态。

## 6. ESP32-S3 网关模块

| 文件 | 职责 |
| --- | --- |
| `firmware/esp32s3_gateway/main/gateway_main.c` | Wi-Fi、RA8 UART、HTTPS 批次、命令和健康状态 |
| `firmware/esp32s3_gateway/main/media/camera_stream.[ch]` | OV5640 JPEG 采集和按订阅上传 |
| `firmware/esp32s3_gateway/main/media/mic_stream.[ch]` | S3 预览麦克风 PCM 上传 |
| `firmware/esp32s3_gateway/main/media/media_protocol.[ch]` | S3MD 二进制媒体帧和控制消息 |
| `firmware/esp32s3_gateway/sdkconfig.defaults` | ESP-IDF 默认配置 |
| `firmware/esp32s3_gateway/partitions.csv` | Flash 分区 |
| `firmware/esp32s3_gateway/main/gateway_secrets.example.h` | 不含真实密钥的配置模板 |

真实 Wi-Fi 和设备密钥文件不得进入 Git 或清单。S3 的 RA8 UART 接收应先于网络和媒体初始化，
并持续发送 `gateway_ready`，防止双端冷启动时 RA8 先发送大帧造成接收环形缓冲溢出。

## 7. RA8-S3 协议和共享内存

| 接口 | 权威文件 | 说明 |
| --- | --- | --- |
| 双核公共类型 | `common/*.h` | 公共错误、状态和基础 IPC/UART 定义 |
| 双核共享 SRAM | `Multicore_top_CPU[01]/src/thread_code/*_ipc_shared.h` | 热阵列、压力、温度、SD、音频和摄像头邮箱 |
| RA8-S3 帧协议 | `Multicore_top_CPU0/src/thread_code/ra8_esp32_protocol.[ch]` | 状态、矩阵、告警、命令和 ACK |
| S3 适配协议 | `tools/ra8_esp32_protocol/ESP32_ADAPTER_PROTOCOL.md` | 网关字段和冷启动握手约束 |
| PC 协议解析 | `tools/ra8_esp32_protocol/ra8_protocol.py` | CH340 调试时的权威解析实现 |

双核共享头必须在 CPU0/CPU1 中保持字段顺序、大小、共享地址和发布顺序一致。RA8-S3 协议
变更必须同步修改 RA8、S3、PC 解析器和协议文档。

## 8. 构建与烧录产物

| 产物 | 生成方式 | 用途 |
| --- | --- | --- |
| `Multicore_top_CPU0/Debug/Multicore_top_CPU0.elf` | CPU0 make/e2 studio | M85 调试 |
| `Multicore_top_CPU0/Debug/Multicore_top_CPU0_internal.srec` | `tools/build_vision_flash_images.ps1` | CPU0 内部 Flash |
| `Multicore_top_CPU0/Debug/vision_models_ospi0_cs0.bin` | `tools/build_vision_flash_images.ps1` | 外部 OSPI 视觉模型 |
| `Multicore_top_CPU1/Debug/Multicore_top_CPU1.elf/.srec` | CPU1 make/e2 studio | M33 调试和烧录 |
| ESP32-S3 binaries | `idf.py build` | S3 bootloader、分区和应用 |

构建二进制可再生且通常被 Git 忽略，因此不写入逐文件源码 CSV。CPU0 含外部 OSPI 地址的组合 SREC
不能作为普通内部 Flash 文件烧录，必须使用内部 SREC 和 OSPI BIN 分开烧录。

## 9. 当前工作区说明

清单以当前工作区为准，包含尚未提交但实际参与 RA8/S3 构建的源码，并在 CSV 的 `git_state`
列中标明。正式归档前应先完成双核、S3 和串口闭环验证并提交 Git，然后重新执行：

```powershell
python tools/generate_ra8_s3_inventory.py
```
