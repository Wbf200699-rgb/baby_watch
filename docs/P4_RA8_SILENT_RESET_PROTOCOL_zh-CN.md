# P4 控制 RA8P1 静默复位协议

本文定义 P4 对 RA8P1 的三条复位路径：

1. UART 正常时发送 `silent_reset`，RA8 回执后自行复位。
2. UART 失联但 CPU0 调度器仍运行时，由 P4 的开漏 GPIO 拉低扩展排针
   `P7-9/P712`，RA8 固件去抖后执行 `NVIC_SystemReset()`。
3. CPU0 完全死锁时，只能使用看门狗或另行焊接真正的 `RESET_n` 硬件线。

三条路径都会被 RA8 判为暖启动，只跳过自动播放的
`/sd0/MUSIC/BOOT.WAV`。冷上电仍播放启动音，启动完成后显式
`play_boot` 命令仍然有效。

## 1. 杜邦线软复位接线（当前默认）

无需焊接，按下表连接：

| P4X-EYE | RA8P1 扩展板 | 说明 |
| --- | --- | --- |
| J22 第 5 脚 / GPIO10 | P7 第 9 脚 / P712 | 低有效软复位请求 |
| GND | 任一 GND 排针 | 必须共地 |

RA8 端建议外接 `10 kOhm` 上拉到 RA8 的 `3V3`。固件同时启用了 P712
内部上拉，但外部上拉能让 P4 或 RA8 尚未启动时的电平更确定。

电气和时序规则：

- P4 GND 与 RA8 GND 必须共地；
- P4 GPIO10 必须是 3.3 V 开漏输出；
- 正常状态以及 P4 自身启动期间必须为高阻；
- 复位时拉低 150 ms，然后释放为高阻；
- 禁止推挽驱动高电平，禁止接入 5 V；
- P4 启动后等待 30 秒才允许自动触发；
- RA8 连续 10 秒没有状态帧才触发；
- 两次自动触发至少间隔 60 秒。

RA8 启动后先观察 P712 为高电平，并等待 5 秒；随后连续低电平约
50 ms 才执行 `NVIC_SystemReset()`。这可避免插拔和两端上电顺序造成
误复位。

这条杜邦线是软件请求输入，不是真正的 MCU `RESET_n`。它可以处理
UART 任务、协议状态机或 P4 链路失效，但 CPU0 调度器完全停止、全局
关中断或 SRAM 严重损坏时无法生效。

### 1.1 可选的真正硬复位

RA8 的真正 `RESET_n` 只可从核心板 J1 第 75 脚、扩展板 P1 第 75 脚
或 SW203 复位按键的低有效网络焊接引出。扩展板没有把它引到普通功能
排针。若需要全核死锁也能恢复，推荐使用 2N7002 等 N 沟道 MOSFET：

```text
P4 GPIO -- gate
             |
          2N7002
             |
drain -- RA8 RESET_n
source - common GND
```

栅极增加下拉电阻，使 P4 上电尚未配置 GPIO 时 MOSFET 保持关闭。此时
P4 驱动的是 MOSFET 栅极，不是直接驱动 `RESET_n`：P4 GPIO 应配置为
3.3 V 推挽输出，高电平使 MOSFET 导通并复位 RA8，低电平使 MOSFET
截止并释放 RA8。不要把 MOSFET 栅极配置成开漏。MOSFET 漏极对 RA8
仍是开漏行为，始终不能向 `RESET_n` 主动灌入高电平。

这条可选焊接线直接进入 RA8 硬件复位网络，因此 UART、FreeRTOS 甚至
两个 RA8 核心都卡死时仍然有效。它与当前 P7-9/P712 杜邦线不是同一
信号，不得混淆。

## 2. 在线 UART 复位

UART 参数和帧封装保持现有协议不变：

```text
AA 55 | VER=01 | TYPE | SEQ_BE16 | LEN_BE16 | PAYLOAD | CRC16_BE16
```

P4 发送类型 `0x20 CMD`，载荷必须与以下任一种完全一致：

```text
silent_reset
```

```json
{"cmd":"silent_reset"}
```

RA8 先发送类型 `0x21 CMD_RESULT`：

```json
{"cmd":"silent_reset","ok":true,"request_sequence":4660,"reset_delay_ms":250}
```

- `request_sequence` 是 P4 请求帧外层的 16 位序号；
- 回执帧自身仍使用 RA8 自己的发送序号；
- RA8 等待 UART 发送完成，然后在 250 ms 后发出 `SYSRESETREQ`；
- 即使回执发送失败，RA8 仍在延时后复位。

P4 收到匹配的 `request_sequence` 后应停止重发，并等待 RA8 状态流先
中断、再恢复。一次请求只执行一次，P4 侧至少保持 5 秒冷却时间。

请求测试向量，外层序号为 `0x1234`：

```text
AA 55 01 20 12 34 00 16
7B 22 63 6D 64 22 3A 22 73 69 6C 65 6E 74 5F 72 65 73 65 74 22 7D
D0 CF
```

## 3. UART 离线兜底

建议 P4 判定连续 2 秒没有合法 `0x10 STATUS_UPLOAD` 后先进行一次短暂
重连。如果仍离线：

1. 确认已超过 30 秒启动保护期且状态离线超过 10 秒；
2. 确认距离上次触发已超过 60 秒；
3. 将 GPIO10/P712 开漏请求线拉低；
4. 保持 150 ms；
5. 释放为高阻；
6. 等待 RA8 完成重启和状态恢复，不在启动窗口重复拉低。

该路径没有 UART 回执。P712 长时间保持低电平时 RA8 每次启动都可能
再次收到请求，因此 P4 必须使用定时器保证脉冲最终释放。

## 4. 冷启动与暖启动判定

只有 CPU1 在 `BSP_WARM_START_POST_C` 中读取 CWSF 和复位原因，然后把
CWSF 置为 1。此时 `SystemRuntimeInit` 已完成普通 `.bss` 初始化，但所有
RTOS 任务尚未启动。CPU0 不读取或写入 CWSF，避免双核竞争。

- 真正掉电后的 POR：CWSF 为 0，自动播放 `BOOT.WAV`；
- `RESET_n`、`SYSRESETREQ`、看门狗、SW203 和调试器复位：CWSF 保持
  为 1，自动启动音被抑制；
- 静默只影响开机自动播放，不禁用后续扬声器功能。

烧录后若只按调试器 Reset，没有断电，启动会按暖启动处理。验证冷启动
音时必须完整断电再上电。

## 5. J-Link 可观测状态

为避免 POR 后首次访问未初始化 ECC SRAM，固件不会在 pre-C 阶段读写
自定义固定 SRAM 快照。CPU1 先把本次启动状态保存在已初始化的普通
`.bss`，音频线程初始化后再在 `AUDIO_IPC` 中公布：

| 地址 | 字段 |
| --- | --- |
| `0x221D3184` | boot_flags |
| `0x221D3188` | boot_capture_count |
| `0x221D318C` | silent_boot_count |
| `0x221D3190` | boot_cold_warm_flag |
| `0x221D3194` | boot_rstsr0 |
| `0x221D3198` | boot_rstsr1 |
| `0x221D319C` | boot_rstsr3 |

`boot_flags` 位定义：

| 位 | 含义 |
| --- | --- |
| 0 | 本次启动状态有效 |
| 1 | 冷启动 |
| 2 | 暖启动 |
| 3 | 本次自动启动音已抑制 |
| 4 | RSTSR1 指示软件复位 |

`boot_capture_count` 固定为 1，`silent_boot_count` 在本次为暖启动时为
1，否则为 0。暖启动正常完成时常见 `boot_flags=0x0D`；由在线
`SYSRESETREQ` 触发时常见值为 `0x1D`。冷启动正常值为 `0x03`。

在线软件复位后可在第一台电脑运行：

```powershell
python tools/read_silent_reset_jlink.py --expect-suppressed --expect-software-reset
```

## 6. P4 验收项

1. 在线命令得到 `0x21`，且 `request_sequence` 与请求一致。
2. 在线复位后状态流恢复，RA8 不播放启动音。
3. 断开 UART 或停止状态流后，GPIO10 在保护窗口结束后产生一次约
   150 ms 低脉冲，RA8 自动恢复。
4. P7-9/P712 触发后不播放启动音。
5. 完整断电再上电仍播放启动音。
6. 示波器确认 P4 GPIO10 从不主动推高 P712，且低脉冲约 150 ms；若
   另接真正 RESET_n 的 MOSFET，确认 gate 为推挽高/低控制。
7. 连续故障不会绕过 60 秒冷却形成复位风暴。
8. 停住 CPU0 调度器时，明确记录 P712 软件请求不能代替真正 RESET_n
   或独立硬件看门狗。
