# SCD41 CO₂ 传感器集成说明

## 1. 集成结论

SCD41 使用独立的 `IIC1`，不与现有 TMP117/PCA9548A 的 `IIC2` 共用驱动实例：

| 信号 | 扩展板连接 | RA8P1 引脚 | FSP 外设 |
| --- | --- | --- | --- |
| SDA | P6-38 | P511 | IIC1/SDA1 |
| SCL | P6-40 | P512 | IIC1/SCL1 |
| 3V3 | P6-1 或 P6-2 | 3.3 V 电源 | — |
| GND | P10 任一 GND | 地 | — |

上述针脚来自《瑞萨 CPKHMI-RA8P1 扩展板原理图》的 P6/P10
网络标注。接线前仍应使用万用表确认扩展板排针方向和模块丝印，
不要按排针外观猜测奇偶脚。

## 2. 电气注意事项

- SCD41 的 I²C 7 位地址固定为 `0x62`，本项目总线频率为 100 kHz。
- SDA、SCL 分别以 4.7–10 kΩ 上拉到 3.3 V。若模块板已经装有上拉电阻，
  应计算并联后的等效阻值，避免重复安装造成过强上拉。
- 在传感器供电端就近放置 100 nF 陶瓷电容和 10 µF 储能电容。
- 以 3.3 V 供电时，应按约 205 mA 的峰值电流和低噪声电源设计，
  不要从驱动能力不明的 GPIO 供电。
- RA8P1 I/O 只能接 3.3 V 逻辑。模块若自带电平转换或稳压器，应先核对
  模块原理图，确认 SDA/SCL 上拉电压不会高于 3.3 V。
- SCD41 的温湿度用于 CO₂ 补偿和环境遥测，不替代 TMP117 床面温度；
  本次不设置未经产品验证的 CO₂ 报警阈值。

电气与时序基准见 Sensirion
[SCD4x 数据手册（D1，v1.7）](https://sensirion.com/media/documents/48C4B7FB/67FE0194/CD_DS_SCD4x_Datasheet_D1.pdf)。
用户提供的
`C:\Users\32792\Downloads\SCD40模块资料.zip` 已核对：其中 Sensirion
SCD40/SCD41 数据手册为 2021 年 4 月 v1.1，另含 SparkFun 模块资料。
该版本明确要求 Standard mode 100 kHz、上电到 idle 最长 1000 ms，给出的
外部上拉示例为 10 kΩ，并列出 3.3 V 下约 205 mA 峰值电流；这些要求与本次
配置一致。驱动上电等待 1100 ms，给最坏 1000 ms 启动时间留出余量。

## 3. FSP 与线程配置

CPU1 的 `configuration.xml` 配置如下：

| 配置项 | 值 |
| --- | --- |
| FSP 驱动 | `r_iic_master` |
| 实例 | `g_scd41_i2c` |
| 通道 | IIC1 |
| 引脚 | P511/SDA1、P512/SCL1 |
| 地址 | 7 位 `0x62` |
| 速率 | Standard mode，100 kHz |
| 回调 | `scd41_i2c_callback` |
| FreeRTOS 线程 | `scd41_thread` |
| 线程优先级 | 3 |
| 线程栈 | 4096 字节 |

修改 FSP 配置后必须重新生成工程内容，再编译 CPU1。应用实现放在
`Multicore_top_CPU1/src`，不要直接维护 `ra_gen` 中的生成文件。

## 4. 驱动流程与数据有效性

CPU1 上电先等待 1100 ms，再按 SCD4x 官方流程使传感器回到已知状态，
读取并校验序列号，随后发送 `start_periodic_measurement`。周期测量每约
5 秒产生一组新数据。
线程轮询 data-ready 状态，只有三个 16 位结果的 CRC-8 全部正确时才发布
新的 CO₂、温度和相对湿度值。

关键协议参数：

| 项目 | 值 |
| --- | --- |
| 启动周期测量 | `0x21B1` |
| 查询 data ready | `0xE4B8` |
| 读取测量 | `0xEC05` |
| 停止周期测量 | `0x3F86`，随后等待 500 ms |
| 重新初始化 | `0x3646` |
| 读取序列号 | `0x3682` |
| CRC-8 | 多项式 `0x31`，初值 `0xFF` |

温度以 `m°C` 发布，相对湿度以 `m%RH` 发布，避免在 MCU 端使用浮点：

```text
temperature_milli_c = -45000 + 175000 * raw_temperature / 65535
humidity_milli_percent = 100000 * raw_humidity / 65535
```

连续通信失败会进入恢复流程；恢复期间保留最后一组完整的有效值，同时更新
状态和错误计数。CRC 错误不会把部分新数据写入共享邮箱。

## 5. 共享 SRAM

SCD41 邮箱固定在 `0x221D0180`，区域大小 `0x80` 字节。CPU1 是唯一写者，
CPU0 是读者。CPU1 用 `publish_lock` 奇偶序列锁包住整帧更新：写入期间为奇数，
完整载荷和 `sequence` 写完并执行内存屏障后提交为偶数。CPU0 融合线程、状态协议
和 CPU1 日志线程都只使用前后锁值相同的稳定快照；若三次有界重试仍遇到发布，
则保留上一帧，不暴露跨两次发布的混合字段。

状态枚举：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `EMPTY` | 尚未初始化 |
| 1 | `STARTING` | 正在初始化 |
| 2 | `MEASURING` | 已启动周期测量，等待新样本 |
| 3 | `SAMPLE_OK` | 最近一次样本完整且 CRC 正确 |
| 4 | `NOT_FOUND` | 初始化/探测未找到传感器 |
| 5 | `CRC_ERROR` | 测量字 CRC 错误 |
| 6 | `I2C_ERROR` | I²C 事务失败 |
| 7 | `TIMEOUT` | I²C 事务超时 |
| 8 | `RECOVERING` | 正在重同步传感器 |

SD 系统日志在 TMP117 字段之后记录：

```text
scd_seq,scd_status,scd_co2_ppm,scd_temp_mc,scd_rh_mp,scd_ready,
scd_crc_err,scd_i2c_err,scd_timeout,scd_recovery,scd_fsp,scd_last_good
```

## 6. RA8、网关和服务器字段

RA8 `STATUS_UPLOAD` 顶层增加 `scd`：

```json
{
  "scd": {
    "seq": 12,
    "s": 3,
    "co2": 735,
    "t_mc": 24620,
    "rh_mp": 51340,
    "ready": 1,
    "crc_err": 0,
    "i2c_err": 0,
    "to": 0,
    "rec": 0,
    "fsp": 0,
    "last": 123456
  }
}
```

网关将它规范化为 `sensors.environment`，保留上述字段名。多模态
`sources/valid/faults` 的 bit 5 表示环境传感器。环境数据超过约 15 秒没有
成功更新时标记为失效/故障，但 SCD41 单独故障只降低系统健康度，不应让核心
婴儿监护链路直接进入 blind。

`ready` 记录最近一次 data-ready 查询结果：读到新样本的发布周期为 1，
后续等待周期会回到 0；它不是传感器健康位。健康与新鲜度应结合 `s` 和
`last` 判断。RA8 协议中的 `last` 已由融合层换算到 CPU0 本地 tick 域，可与
顶层 `tick` 直接比较；CPU1 单独复位不会造成永久失效误报。共享邮箱和 SD
日志中的 `last_good_tick` 仍保留 CPU1 原始 tick。即使 I²C 仍可应答，连续
15 秒没有新样本也会触发驱动恢复。

服务器将 `sensors.environment` 当作可选、可扩展字典处理；旧固件没有
`scd` 时，网关和服务器都必须继续正常工作。

## 7. 上电验收

1. 断电检查 3V3 与 GND 无短路，确认 SDA/SCL 上拉到 3.3 V。
2. 上电后确认 IIC1 能访问 `0x62`，序列号三个字均通过 CRC。
3. 等待至少 5 秒，确认 `status=3`、`sequence` 和 `last_good_tick`
   持续更新。
4. 观察 10 分钟，确认 `crc_error_count`、`i2c_error_count` 和
   `timeout_count` 不持续增长。
5. 在 SD CSV、RA8 `scd`、网关 `sensors.environment` 和服务器界面比较
   同一时刻数据，确认单位未被二次换算。
6. 断开再接回 SDA 或传感器供电，确认状态进入错误/恢复并能自动回到
   `SAMPLE_OK`。
