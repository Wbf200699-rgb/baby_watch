# BME688 接入与 P4 适配说明

## 1. 接线与总线

BME688 与现有 SCD41 串接在 CPU1 的同一条 IIC1 总线上。该总线为
100 kHz，并由同一个环境线程串行访问，因此不需要第二个 I²C 实例或
额外互斥锁。

| BME688 模块引脚 | RA8 扩展板 | 说明 |
| --- | --- | --- |
| `SCK` | P512 / IIC1 SCL | 时钟 |
| `SDI` | P511 / IIC1 SDA | 数据 |
| `SDO` | GND 或 3.3 V | GND 选择 `0x76`，3.3 V 选择 `0x77` |
| `CS` | 3.3 V | 必须拉高以选择 I²C 模式 |
| `VCC/VIN` | 3.3 V | RA8 侧逻辑电平为 3.3 V |
| `GND` | GND | 与 RA8、P4 共地 |

固件每次事务都会重新选择 7-bit 从地址：

- SCD41：`0x62`
- BME688：启动时依次探测 `0x76`、`0x77`

不要让 `CS` 悬空，也不要向 P511/P512 输入 5 V。若两个模块板上都带
I²C 上拉，需确认并联后的等效上拉阻值不过小。

## 2. 固件结构

CPU1 的 `scd41` 环境线程配置为优先级 3、4096 字节栈，按以下顺序循环：

1. 轮询 SCD41；
2. 触发 BME688 forced-mode 测量；
3. 等待转换和 300 °C / 100 ms 气体加热；
4. 读取 Bosch 补偿后的温度、湿度、气压和原始气阻；
5. 通过同一个 seqlock 原子发布跨核快照。

BME688 使用 Bosch 官方 BME68x SensorAPI v4.4.8，配置为温度 ×2、湿度
×2、气压 ×4、IIR 3。连续三次失败后先重建 BME688 实例；若重新探测仍
失败，则通过 SCD41 总线拥有者执行 IIC1 关闭、9 个 SCL 解锁脉冲、重新
打开和 SCD41 同步，再重试 BME688。BME688 从不直接操作控制器生命周期，
避免两设备互相破坏。每次掉线过程最多执行一次共享总线恢复，之后仍每
5 秒普通探测；探测成功后才重新允许下一次总线恢复。这样既能自动恢复
瞬态控制器故障，又不会让未安装或未供电的模块周期性中断 SCD41。

官方来源：

- <https://github.com/boschsensortec/BME68x_SensorAPI>
- <https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme688-ds000.pdf>

## 3. 跨核邮箱

环境邮箱位于 `0x221D0180`，固定为 128 字节。原 SCD41 字段之后追加
9 个 32-bit BME688 字段：

| 字偏移 | 字段 | 编码 |
| --- | --- | --- |
| 23 | `bme_status` | 状态枚举 |
| 24 | `bme_i2c_address` | `0x76` 或 `0x77` |
| 25 | `bme_temperature_milli_c` | 0.001 °C，有符号 |
| 26 | `bme_humidity_milli_percent` | 0.001 %RH |
| 27 | `bme_pressure_pa` | Pa |
| 28 | `bme_gas_resistance_ohm` | Ω |
| 29 | `bme_flags` | 有效位 |
| 30 | `bme_last_good_tick` | CPU1 FreeRTOS tick |
| 31 | `bme_error_count` | 累计错误次数 |

状态值：

| 值 | 含义 |
| --- | --- |
| 0 | 未初始化 |
| 1 | 启动中 |
| 2 | 等待测量 |
| 3 | 最近样本正常 |
| 4 | 未找到传感器 |
| 5 | 数据或驱动错误 |
| 6 | I²C 错误 |
| 7 | 超时 |
| 8 | 恢复中 |

`bme_flags`：

- bit 0：本轮有新数据；
- bit 1：芯片声明气阻有效；
- bit 2：加热器已稳定。

没有新数据但上一样本仍有效时，状态保持 3，只清 bit 0。

## 4. RA8 到 P4

RA8 的 `STATUS_UPLOAD` 顶层新增：

```json
{
  "bme": {
    "s": 3,
    "addr": 118,
    "t_mc": 24125,
    "rh_mp": 48500,
    "p_pa": 100825,
    "gas_ohm": 125400,
    "flags": 7,
    "last": 123450,
    "err": 0
  }
}
```

其中 `last` 已在 CPU0 转换为与顶层 `tick` 相同的时钟域，`err` 是累计
错误次数。

P4 适配时应把 RA8 顶层 `bme` 放到：

```text
sensors.environment.bme688
```

原有 SCD41 字段继续直接保留在 `sensors.environment` 下。服务端也接收
迁移别名 `sensors.bme688`，但会立即归一到上述规范位置。

完整帧格式、状态字段和 P4 验收要求见
`tools/ra8_esp32_protocol/ESP32_ADAPTER_PROTOCOL.md`。

## 5. 气体字段限制

`gas_ohm` 是传感器补偿后的原始气敏电阻。当前固件没有运行 Bosch BSEC，
因此它不是 VOC、VSC、eCO₂ 或 IAQ。只有 flags bit 1 和 bit 2 同时置位时，
界面才应把该气阻标为当前有效；即使气阻仍在预热，温湿度和气压仍可独立
显示。

## 6. J-Link 验收

烧录两核后，在第一台开发机仓库根目录执行：

```powershell
python tools\monitor_environment_jlink.py `
  --samples 24 `
  --interval-ms 1000 `
  --output test_results\environment_jlink
```

通过条件包括：

- 邮箱 magic 正确且 sequence 持续增长；
- BME688 状态为 3，地址为 `0x76` 或 `0x77`；
- 温湿度、气压、气阻在合理编码范围内；
- `gas_valid` 和 `heater_stable` 至少出现一次；
- BME688 `last` 持续增长且错误计数不新增；
- SCD41 同时保持状态 3，读取计数继续增长。
