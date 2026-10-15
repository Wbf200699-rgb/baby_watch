# baby_watch · 婴儿智能监护系统

面向婴幼儿看护场景的软硬件协同项目，覆盖传感器采集、边缘风险判断、实时音视频传输、服务端管理和 Android 远程监护。

系统将视觉、压力、热成像、床面温度和声音等信息汇总为宝宝状态及分级提醒，帮助看护者查看异常原因，并通过手机完成音乐播放、暖光和拍抚等人工安抚操作。

## 项目概览

| 维度 | 内容 |
| --- | --- |
| 使用场景 | 婴儿床状态监护、异常提醒、远程查看与安抚 |
| 硬件平台 | Renesas RA8P1 双核主控、ESP32-P4X-EYE 网关 |
| 感知能力 | 视觉与哭声、压力分布、热成像、五点床温、温湿度与 CO₂ |
| 服务端 | Python、FastAPI、SQLite、实时数据接口与网页控制台 |
| 手机端 | Kotlin、Jetpack Compose、StateFlow、OkHttp |
| 工程验证 | 协议测试、客户端单元测试、服务端测试与 GitHub Actions |

## 主要功能

- **状态总览**：查看在床状态、活动情况、哭声、环境与当前风险等级。
- **远程监护**：通过服务端查看实时画面并监听声音。
- **传感器可视化**：展示五点床面温度、热成像、压力分布及环境数据。
- **分级告警**：呈现安全事件、判断依据、确认与恢复过程。
- **人工安抚**：独立的摇篮曲与 AI 故事入口，以及暖光、拍抚控制。
- **电脑备用服务**：公网服务不可用时，可手动将网关和 App 切换到同一局域网电脑服务。

## 技术亮点

| 设计 | 工程价值 |
| --- | --- |
| 多模态证据融合 | 结合视觉、压力与热成像，降低对单一路径的依赖 |
| 端侧安全判断 | RA8P1 执行融合与事件状态机，基础判断不依赖云端 AI |
| 实时链路隔离 | 遥测、矩阵、视频和音频分开传输；最新帧队列减少旧数据积压 |
| 控制回读确认 | 设备开关依据硬件遥测确认，区分命令接收与实际执行 |
| 异常恢复与端点隔离 | 退避重连、超时处理、旧连接数据过滤，以及云端/本地配置分离 |
| 数据与运行观测 | 训练采集、事件时间线、CPU/NPU 利用率与内存余量记录 |

AI 用于状态解释与故事生成；安全告警以边缘融合结果为准。热成像温度属于辅助筛查信息，系统定位为实验原型。

## 系统架构

```text
摄像头 / 麦克风 / 压力 / 热成像 / 床温 / 环境传感器
                         ↓
                RA8P1 双核采集与边缘判断
                         ↓
                  ESP32-P4 网关
                         ↓
              FastAPI / SQLite 服务端
                    ↙           ↘
              网页控制台      Android App
```

## 源码导航

| 模块 | 入口 |
| --- | --- |
| 当前维护的 Android App | [app/](app/) |
| RA8P1 双核固件 | [CPU0](baby_monitor/Multicore_top_CPU0/)、[CPU1](baby_monitor/Multicore_top_CPU1/)、[共享代码](baby_monitor/common/) |
| ESP32-P4 网关 | [p4x_eye/gateway](baby_monitor/p4x_eye/gateway/) |
| ESP32-S3 / 早期网关 | [firmware/](baby_monitor/firmware/) |
| 服务端及网页 | [server/](baby_monitor/server/) |
| 视觉训练 | [vision_training/](baby_monitor/vision_training/) |
| 哭声训练 | [ml_training/](baby_monitor/ml_training/) |
| 融合标定 | [fusion_training/](baby_monitor/fusion_training/) |
| 协议与演示工具 | [tools/](baby_monitor/tools/) |
| 原理图与完整原始工程 | [baby_monitor/](baby_monitor/) |

根目录的 `app/` 是此前持续优化的客户端。`baby_monitor/mobile/baby_watch/` 是随整套工程保留的上游客户端快照；日常 Android 开发与 CI 使用根目录工程。

## 运行与验证

**Android App**：使用 Android Studio、JDK 17 或更新版本、Android SDK 36。打开仓库根目录，执行：

```powershell
.\gradlew.bat testDebugUnitTest lintDebug assembleDebug
```

支持 Android 7.0 及以上。APK 输出至 `app/build/outputs/apk/debug/app-debug.apk`，登录凭据在 App 设置中填写，源码不包含真实密码或设备密钥。

**服务端**：进入 [服务端运行说明](baby_monitor/server/README.md)，使用 Python 虚拟环境安装依赖并启动 FastAPI。默认本地服务端口为 `8088`。

**硬件与现场部署**：参考 [完整工程说明](baby_monitor/README.md) 和 [电脑备用服务指南](baby_monitor/docs/DEMO_FALLBACK_GUIDE_zh-CN.md)。嵌入式工程需要对应的 e² studio / FSP 和 ESP-IDF 环境。

已验证：Android 37 项业务单元测试通过，Lint 0 错误，APK 构建成功；服务端 159 项测试和网页历史逻辑 7 项测试通过。测试环境与完整结果见 [验证记录](docs/PROJECT_STATUS_zh-CN.md)，硬件联调与软件自动化测试分别记录。

[查看持续集成与构建产物](https://github.com/Wbf200699-rgb/baby_watch/actions/workflows/android.yml)

## 项目资料

- [项目详细说明](docs/PROJECT_OVERVIEW_zh-CN.md)
- [App 架构与通信设计](docs/APP_ARCHITECTURE_zh-CN.md)
- [6 分钟演示顺序](docs/DEMO_6_MIN_zh-CN.md)
- [全流程测试方案](docs/CURRENT_FULL_FLOW_TEST_AND_DEMO_PLAN_zh-CN.md)
- [项目整理与验证记录](docs/PROJECT_STATUS_zh-CN.md)
- [完整工程来源与导入记录](docs/UPSTREAM_IMPORT_zh-CN.md)

## 版本与来源

完整工程来自 [suchen2333/baby_monitor](https://github.com/suchen2333/baby_monitor)，导入基线为 `agent/thermal-quality-reset-header` 分支的 `fbe6663`。保留原文件及其中的版权和许可说明；本仓库同时维护 Android 客户端的迭代与验证记录。
