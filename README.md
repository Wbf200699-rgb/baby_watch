# baby_watch · 婴儿智能监护 Android 客户端

基于 Kotlin 与 Jetpack Compose 的智能硬件配套 App，连接婴儿监护服务端，将实时音视频、多模态安全状态、传感器图像与人工安抚控制整合到手机端。

本仓库包含 **Android 客户端源码、自动化测试及项目文档**。RA8P1 双核固件、ESP32-P4 网关与 FastAPI 服务端位于配套项目 [baby_monitor](https://github.com/suchen2333/baby_monitor)。

## 项目解决的问题

家长需要同时了解宝宝是否在床、是否哭闹、周围环境以及异常发生的原因。客户端把这些分散的数据组织成状态总览、实时监护、传感器可视化和事件时间线；当网络异常或数据失效时，明确展示连接状态和证据可用性。

## 功能与页面

| 页面 | 功能 |
| --- | --- |
| 首页 | 宝宝状态、风险等级、哭声与环境摘要、多模态判断依据 |
| 监护 | 手动开启实时视频与音频、视觉结果、暖光与拍抚控制、独立摇篮曲和 AI 故事卡片 |
| 洞察 | 本地监护趋势、事件时间线及服务端 AI 辅助解释 |
| 传感器 | 四角和中心五点床温、热成像与温度摘要、压力分布、SCD41/BME688 环境数据 |
| 设置 | 设备连接、监护通知、云端和电脑备用服务的手动切换 |

## 实现重点

- **实时通信分工**：HTTP 用于配置、查询和控制，SSE 用于状态与事件，WebSocket 用于视频和音频。
- **弱网与背压处理**：请求超时、有限重试、连接退避、媒体看门狗；矩阵和视频采用最新帧队列，减少积压。
- **端点切换隔离**：分别保存云端和本地配置，切换时关闭旧连接、清理旧状态，并通过 generation 校验过滤旧端响应与帧。
- **可解释安全展示**：解析融合来源有效位及各路证据；未知、失效和过期数据不展示为正常。安全事件按生命周期去重，用户“已查看”与事件解除分开。
- **设备回读确认**：暖光与拍抚开关以硬件遥测为准，不把服务器接收命令等同于设备已经执行。
- **凭据保护**：密码使用 Android Keystore 加密保存；云端要求 HTTPS，本地 HTTP 仅接受私网或回环字面 IPv4。

## 系统位置

```text
摄像头 / 麦克风 / 压力 / 热成像 / 床温 / 环境传感器
                        │
                   RA8P1 双核
              边缘推理、融合与安全事件
                        │ UART
                 ESP32-P4 网关
                        │ HTTPS / WSS
                 FastAPI 服务端
                        │ HTTP / SSE / WebSocket
                  baby_watch App
```

本地安全决策由 RA8P1 执行；App 负责展示、远程操作和通知。服务端 AI 用于状态解释和故事生成，不替代硬件安全决策。

## 技术栈

| 类别 | 当前配置 |
| --- | --- |
| 语言与界面 | Kotlin、少量 Java、Jetpack Compose、Material 3 |
| 状态与异步 | StateFlow、Kotlin Coroutines、Channel |
| 网络 | OkHttp 4.12.0、HTTP、SSE、WebSocket |
| 本地存储 | SharedPreferences、Android Keystore、轻量历史缓存 |
| 构建 | Gradle Wrapper 9.1.0、Android Gradle Plugin 9.0.1 |
| Android | minSdk 24，compileSdk / targetSdk 36 |
| 测试 | JUnit 4、协议解析与网络/矩阵安全测试、Android Lint |

具体依赖以 [版本目录](gradle/libs.versions.toml) 为准。

## 本地运行

1. 安装 Android Studio，使用 JDK 17 或更新版本，并安装 Android SDK 36。
2. 克隆本仓库后，用 Android Studio 打开根目录并完成 Gradle 同步。
3. 让 Android Studio 生成 `local.properties`；该文件不提交到 Git。
4. 构建并安装到 Android 7.0 及以上手机，在设置中填写服务地址和登录信息。

```powershell
git clone https://github.com/Wbf200699-rgb/baby_watch.git
cd baby_watch
.\gradlew.bat testDebugUnitTest lintDebug assembleDebug
```

macOS / Linux 使用 `./gradlew`。Debug APK 位于 `app/build/outputs/apk/debug/app-debug.apk`。

云端默认地址是 `https://renesas.zyxserver.online`，实际可用性取决于配套服务的部署；源码不包含登录密码或设备密钥。当前设备标识默认是 `esp32cam-01`。

### 电脑备用服务（Plan B）

在配套仓库启动电脑服务，手机设置中手动选择电脑端并填写 `http://电脑私网IPv4:8088`。**网关也必须指向同一电脑服务**。本方案不提供自动接管或双端数据库同步。

离线局域网测试时，手机应保持连接该 Wi-Fi；若系统自动切到蜂窝网络，应关闭移动数据后重试。本地 HTTP 不加密传输，勿复用云端密码。

## 目录结构

```text
app/src/main/
├── java/com/example/baby_watch/
│   ├── data/server/       协议模型、仓库、通信、配置、历史与事件策略
│   ├── ui/                Compose 页面、组件与主题
│   ├── notification/      告警通知、铃声与振动
│   └── service/           前台监护服务与日志
└── res/                   图标、主题与 Android 资源
app/src/test/              JVM 单元测试
app/src/androidTest/       Android 仪器测试入口
docs/                      架构、测试、演示和版本整理记录
gradle/                    Wrapper 与依赖版本目录
```

## 文档与验证

2026-10-14 本地验证：**37 项业务单元测试通过，Android Lint 0 错误，Debug APK 构建成功**。Lint 另有 13 项非阻断警告；硬件与真机联调不包含在该结果内。

- [App 架构与通信说明](docs/APP_ARCHITECTURE_zh-CN.md)
- [6 分钟演示顺序](docs/DEMO_6_MIN_zh-CN.md)
- [全流程测试与答辩指南](docs/CURRENT_FULL_FLOW_TEST_AND_DEMO_PLAN_zh-CN.md)
- [本轮整理与验证记录](docs/PROJECT_STATUS_zh-CN.md)

持续集成入口：[Android checks](https://github.com/Wbf200699-rgb/baby_watch/actions/workflows/android.yml)。

## 简历描述参考

婴儿智能监护 Android 客户端：使用 Kotlin、Jetpack Compose、StateFlow 和 OkHttp 实现实时监护界面，接入 HTTP/SSE/WebSocket 数据链路，完成多模态安全事件、传感器可视化与安抚控制交互；针对弱网和服务切换实现退避重连、最新帧队列与端点状态隔离。

以上是代码能力概述；个人职责、团队分工和性能指标应按本人实际贡献与测试记录填写。

本项目是智能监护实验原型。热成像与床面温度用于辅助筛查和状态展示，不能代替医疗测温或诊断；系统仍需人工看护。自动化测试与真机、硬件联调结果分别记录，不互相替代。
