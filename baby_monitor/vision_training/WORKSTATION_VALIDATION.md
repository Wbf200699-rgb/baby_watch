# 视觉原型训练与部署工具链实测

验证日期：2026-07-27。本记录区分“电脑端模型已训练/编译”和“RA8P1 视觉闭环
已上板”两个状态，不能把前者写成后者。

## 本轮设备数据恢复

- SD 卡 `E:\CAMERA` 中找到 1,628 张无红框原图，和
  `ml_training\data\打框数据集.zip` 去除 91 张字节级副本后的 1,628 张图
  一一匹配。
- `recover_boxed_capture_dataset.py` 从压缩包图片提取红框坐标，再应用到同名
  SD 原图；尺寸、文件名和感知哈希不匹配时会拒绝样本。
- 按时间顺序、保护间隔和降采样得到 546 张设备域原图。它们来自同一批连续
  采集，划分不是独立场次，因此以下分类指标偏乐观。
- `baby_sheet` 同时包含面部清楚、转头和被子接触身体等不同语义，未伪造为
  `partial`；当前分类 ROI 也只是婴儿/场景框，不是统一头部框。

## 三个 prototype_v1 模型

| 模型 | 本轮可训练类别 | strict INT8 结果 | 文件大小 |
| --- | --- | ---: | ---: |
| 口鼻遮盖 MobileNetV3-Small 192 | clear、covered、not_visible | 测试 30/32，93.75% | 616,992 B |
| 行为 MobileNetV3-Small 192 | quiet、active、edge_grab、unknown | 测试 44/54，81.48% | 617,600 B |
| 场景 YOLOX-Tiny 224 | infant、head_face、blanket、soft_object；caregiver 无样本 | 最佳 val AP50:95 0.470；训练中最高 AP50 0.740 | 5,337,208 B |

三个 INT8 文件的 SHA-256 依次为：
`1f3bb3368d25ac55591d34b244851792ed9254d8736eeb5e67e48a2da3d35ee7`、
`452a915f0f33b0b1c36f8cd6d5ff4436ea23ca7afdf32adcdddf23e75f1857f5`、
`c1502f28942a2d0dcca5991614040487aa9a8d0b5d6d0c42115f7dd3ccd71068`。

遮挡模型的 `covered` INT8 测试召回率只有 4/6；行为模型的 `active` 召回率只有
5/9。测试图仍和训练图来自相同历史采集批次，所以这些数字只能用于本机原型筛选，
不能作为安全告警准确率或泛化性能。

场景数据还合并了 CribHD 和 SyRIP。最终 COCO 数据为：

- train：2,426 图、4,228 框；
- validation：725 图、1,351 框；
- test：97 图、107 框；
- train 框计数：infant 1,456、head_face 1,108、blanket 562、
  soft_object 1,102、caregiver 0。

## 导出与量化

- NVIDIA GeForce RTX 4070 Laptop GPU 完成 40 轮 YOLOX 训练。
- ONNX 固定输入 `[1,3,224,224]`、输出 `[1,1029,10]`，checker 通过。
- Focus 与检测头布局采用导出等价改写，训练模型本次最大绝对误差
  `1.9073486328125e-06`。
- WSL2 Ubuntu 24.04 完成 ONNX → strict INT8 TFLite；输入为
  `[1,224,224,3] int8`、`scale=1.0`、`zero_point=-128`，输出为
  `[1,1029,10] int8`。
- `validate_tflite.py` 和 `generate_scene_embedded_support.py` 均通过。

## RUHMI / Ethos-U55 实测

官方 MERA `2.6.0+pkg.4815` 轮子要求 CPython 3.10。安装脚本在 Ubuntu 24.04
中用 `uv` 并存安装隔离的 3.10，不替换系统 Python 3.12。

| 模型 | NPU 算子 | CPU 算子 | Vela SRAM | 权重/片上 Flash 报告 |
| --- | ---: | ---: | ---: | ---: |
| 遮挡分类 | 76（100%） | 0 | 252 KiB | 541.28 KiB |
| 行为分类 | 76（100%） | 0 | 252 KiB | 541.91 KiB |
| 场景检测 | 300（100%） | 0 | 784 KiB | 4,565.61 KiB |

场景模型以 `-External` 生成，模型常量节为 `.sdram_from_ospi0_cs1`，工作区为
`.sdram`。RUHMI 编译成功只说明算子和代码生成兼容，并不说明当前 FSP 工程已经
配置这些内存。

## 当前不能宣称“视觉已上板”的原因

当前 CPU0 工程仍同时存在以下硬阻塞：

1. `BSP_CFG_SDRAM_ENABLED=0`、`BSP_CFG_OSPI_B_STARTUP_ENABLED=0`；
2. 工程没有 `rm_ethosu`/Ethos-U 驱动实例；
3. `vision_preprocess_thread` 和 `vision_infer_thread` 仍是挂起占位线程；
4. SCI3 摄像头只把 1,024 字节 JPEG 分块交给 CPU1 写 SD，CPU0 没有完整帧；
5. CPU0 工程没有 JPEG 解码器，也没有 JPEG→RGB→模型张量的实测路径。

在不改动并重新生成用户的 FSP 配置、没有验证外部内存和完整 JPEG 输入前，直接
把 4.65 MiB 常量与 784 KiB arena 塞进现有工程会生成不可启动或不可链接固件。
因此本轮保留完整 TFLite、RUHMI C 代码和预/后处理产物，但没有伪造“已上板”
状态，也没有覆盖 `configuration.xml`。

## 本轮回归

- `vision_training/tests`：21 passed；
- `ml_training/tests`：21 passed；
- `server/tests`：38 passed；
- `node --check server/static/app.js`：通过；
- CPU0 LLVM 构建：通过，新的 SREC 已生成；
- CPU1 LLVM 构建：通过，现有 SREC 保持有效。

CPU0 构建树中一个已跟踪的 FreeRTOS `heap_4.c` 当前被其他工作树改动删除，
本轮没有恢复或覆盖它；验证时明确复用已有 `heap_4.o` 完成增量链接。正式清理
工作树或全量 clean build 前，需要先确认该删除是否为用户有意操作。
