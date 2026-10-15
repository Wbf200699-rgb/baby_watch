# 三模型与多传感器融合落地规格

## 核心边界

三个视觉模型只输出证据，不直接输出最终 App 状态。压力矩阵、HTPA80×64、音频、温湿度和 CO₂ 也只提供各自能可靠观测的证据。

系统是安全风险辅助提醒，不是医疗设备，不判断缺氧、停止呼吸或医学原因。

## 与当前工程一致的任务分配

当前项目已经由 CPU0 的 `fusion_thread_entry.c` 和 `multimodal_fusion.h` 负责安全融合。第一版保持：

```text
CPU0 / Cortex-M85:
  摄像头预处理
  YOLOX-Tiny
  两个 MobileNetV3
  哭声模型
  Ethos-U55 调度
  多传感器融合状态机
  上层控制与机械动作急停

CPU1 / Cortex-M33:
  压力矩阵采集
  HTPA80×64 接收
  温度/环境传感器
  SD 卡、日志和音频底层
  传感器故障与时间戳上报
```

不在引入视觉模型时把融合状态机迁到 CPU1。CPU1 只发布带序号和时间戳的特征快照，CPU0 统一作安全决策。

## 模型证据结构

后续应把当前旧接口：

```c
multimodal_fusion_set_vision_result(
    sleep_confidence,
    face_covered_confidence,
    standing_confidence,
    movement_confidence,
    timestamp_tick);
```

升级为非互斥证据结构，至少包含：

```text
scene:
  infant_present + bbox
  head_present + bbox
  blanket_present + boxes
  soft_object_present + boxes
  caregiver_present

occlusion:
  clear / partial / covered / not_visible probabilities

behavior:
  quiet / active / edge_grab / caregiver / unknown probabilities

geometry:
  infant_to_edge_distance
  head_blanket_overlap
  body_motion_vector

health:
  frame_age_ms
  camera_fault
  model_fault
```

每条证据都必须带采集时间，不可把旧视觉结果当成当前帧。

## 第一版规则

### 婴儿存在

```text
压力存在 + (RGB 检测到婴儿 或 HTPA 检测到热目标)
  -> PRESENT_CONFIRMED

只有一种传感器存在
  -> PRESENT_UNCERTAIN

RGB、压力、HTPA 连续 5 秒均不存在
  -> ABSENT_CONFIRMED
```

摄像头故障时不能因为 RGB 无目标就判离床。

### 清醒、睡眠和过渡

```text
AWAKE:
  active 连续约 3 秒
  或压力活动量明显
  或哭声状态有效

SLEEPING:
  quiet 连续 45 秒
  且压力变化低
  且 10 秒内无哭声
  且婴儿存在证据稳定

证据冲突或模型 unknown:
  TRANSITION
```

清醒进入快，睡眠确认慢。单帧分类结果不能直接切换睡眠。

### 靠边和扒边

床边使用安装后固定 ROI，压力中心使用床垫外侧约 15% 区域：

```text
压力中心靠边 + 婴儿框进入边缘 ROI，持续 2 秒
  -> level2 靠边预警

上述条件 + edge_grab 持续约 1.5 秒
  或身体中心持续向床外移动
  -> level3 扒边/攀爬风险
```

照护者存在时可以抑制部分行为误报，但不能抑制明确 `covered` 风险。

### 口鼻遮盖

```text
partial 持续 2 秒
  -> level2 疑似部分遮挡

covered 持续 1.5 秒
且 blanket/soft_object 与 head_face 明显重叠
  -> level3 口鼻遮盖风险

head_face 存在但 not_visible 持续 5 秒
  -> level2 请检查姿态
```

`not_visible` 不是 `covered`。RGB 也不能证明呼吸正常。

### 传感器故障

- 每个传感器和模型使用独立 stale 超时。
- 摄像头帧、推理结果、压力和热阵列分别检查序号是否增长。
- 视觉链路超过 2 秒无新结果进入 camera/model fault。
- 多个关键存在传感器冲突持续 5 秒进入 `level1`，同时显示具体故障源。

### level3 机械安全动作

进入 `level3` 后立即停止拍一拍、摇篮等机械安抚，只保留声光报警、消息通知和日志。退出 `level3` 需要人工确认或比进入更长的安全恢复时间，不能因单帧恢复自动重启机械动作。

## 调度与内存

Renesas 官方演示的单模型 NPU AI 延迟约为：

- YOLOX-Tiny 224：73 ms；
- MobileNetV3-Small 192：24 ms。

这是单模型 AI-only 数据，不包含摄像头、JPEG 解码、缩放、缓存切换和融合。第一版采用串行 NPU 调度：

```text
场景检测 2～4 FPS
  -> 有 head 才运行遮盖分类 4 FPS
  -> 有 infant 才运行行为分类 1～2 FPS
```

不要同时为三个模型长期保留最大 tensor arena。RUHMI 编译后按实际生命周期复用工作区，并在启用外部 SDRAM/OSPI 后做峰值测量。

## 上板验收

1. 同一组 50 张图在 PyTorch、TFLite、RA8P1 上逐张对比。
2. 检查 BGR/RGB、letterbox padding=114、量化 scale/zero-point 和框坐标反变换。
3. 单模型运行至少 30 分钟，确认序号持续增长。
4. 三模型串行运行至少 2 小时，监控内存、NPU 错误和最坏延迟。
5. 断开摄像头、冻结帧、遮住镜头、制造 UART/传感器超时，确认进入故障而不是输出安全。
6. 用保留测试场次验证夜间、靠边、遮盖和照护者情形。
7. 最后才把视觉证据接入机械动作急停。
