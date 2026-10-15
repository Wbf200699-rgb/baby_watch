# RA8P1 三视觉模型训练基础

> 当前部署模型仍使用 V1 行为类别。下一轮数据采集和重训请以
> [VISION_MULTIMODAL_COLLECTION_PLAN_V2_zh-CN.md](VISION_MULTIMODAL_COLLECTION_PLAN_V2_zh-CN.md)
> 为准：V2 将行为模型改成可观察姿态，`quiet / active / edge_grab` 改由时序多模态融合产生。

本目录把视觉部分固定为三个互不互斥的模型，并为后续 CPU0 规则融合提供证据：

| 模型 | 结构 | 输入 | 类别 |
| --- | --- | --- | --- |
| 场景检测 | YOLOX-Tiny | 224×224（RGB 源图转 BGR 张量） | `infant`、`head_face`、`blanket`、`soft_object`、`caregiver` |
| 口鼻遮盖 | MobileNetV3-Small minimalistic | 192×192 头部 ROI | `clear`、`partial`、`covered`、`not_visible` |
| 行为状态 | MobileNetV3-Small minimalistic | 192×192 婴儿 ROI | `quiet`、`active`、`edge_grab`、`caregiver`、`unknown` |

不要把三个模型改回单一的“睡眠/清醒/靠边/遮盖/无人”分类器。这些状态可以同时发生，必须由时间状态机与压力、HTPA80×64、哭声证据融合。

## 已准备内容

- CribHD、原始 SyRIP、扩展 SyRIP 的官方来源、访问状态和用途登记。
- 可断点续传、安全解压、校验哈希的数据下载器。
- CribHD COCO + SyRIP COCO keypoints + 自采 COCO 的统一数据整理器。
- SyRIP 人体框转 `infant`，并从面部关键点生成带 `weak_annotation` 标记的 `head_face` 框。
- 优先保留公开数据官方划分；若同一来源跨划分则整体隔离到 holdout。自采数据按婴儿、录像、玩偶或场次整体划分，禁止随机拆帧。
- 最终摄像头录像/图片的场次采样、近重复帧过滤和元数据保存。
- 两个 ROI 分类数据集的裁剪、标签校验、场次隔离和审计。
- MobileNetV3-Small 两阶段微调、类别权重、严格 INT8 TFLite 导出和量化后评估。
- YOLOX-Tiny 224 官方训练骨架、ONNX 导出、按 Renesas 参考流程进行 TFLite INT8 量化。
- RUHMI CPU/NPU 编译入口和 TFLite I/O 合约检查。

所有原始数据、虚拟环境、外部仓库和模型产物均被 `.gitignore` 排除。
这台电脑已经完成的实际工具链验证记录见
[WORKSTATION_VALIDATION.md](WORKSTATION_VALIDATION.md)。

## 官方依据

- Renesas YOLOX-Tiny 参考：<https://github.com/renesas/ruhmi-model-zoo/tree/main/vision/object_detection/yolox_tiny>
- Renesas MobileNetV3-Small 参考：<https://github.com/renesas/ruhmi-model-zoo/tree/main/vision/image_classification/mobilenetv3>
- YOLOX 官方实现：<https://github.com/Megvii-BaseDetection/YOLOX>
- CribHD / CribNet：<https://github.com/ostadabbas/CribNet>
- SyRIP / FiDIP：<https://github.com/ostadabbas/Infant-Pose-Estimation>

CribHD 和 SyRIP 官方仓库均写有非商业用途限制。当前可用于课程、研究和原型验证；任何商业化之前必须重新确认数据许可，不要只依据本说明。

## 第一次使用

在 Windows PowerShell 中：

```powershell
cd D:\Projects\e2studio\ra8p1\vision_training
Set-ExecutionPolicy -Scope Process Bypass
.\setup.ps1
```

这台工作站现有 Python 3.12，`setup.ps1` 默认调用 `py -3.12`，不会修改系统 Python。
Renesas RUHMI/MERA 编译器仍应使用其安装说明要求的独立环境；不要把训练环境当成编译器环境。

安装后可分别验证 MobileNetV3 的 strict INT8 转换和 YOLOX 的 CUDA 前向：

```powershell
.\.venv\Scripts\python.exe .\smoke_test_mobilenet.py
.\setup_yolox.ps1
.\.venv_yolox\Scripts\python.exe .\smoke_test_yolox.py --export-onnx
.\setup_conversion_wsl.ps1
```

冒烟模型使用随机权重，只验证工具链、张量形状和量化闭环，不代表模型准确率。
常规回归检查可运行 `.\validate_environment.ps1`。

### 1. 下载公开数据

```powershell
.\.venv\Scripts\python.exe .\download_vision_datasets.py
```

默认下载：

- `CribHD.zip`：611 MB，可直接下载。
- `SyRIP.zip`：313 MB 原始版，可直接下载。

扩展 SyRIP 是可选项：

```powershell
.\.venv\Scripts\python.exe .\download_vision_datasets.py --dataset syrip_expanded
```

扩展版压缩包可下载，但官方要求联系作者取得密码。取得后只在当前 PowerShell 会话设置：

```powershell
$env:SYRIP_ARCHIVE_PASSWORD = "作者提供的密码"
.\.venv\Scripts\python.exe .\download_vision_datasets.py --dataset syrip_expanded
Remove-Item Env:SYRIP_ARCHIVE_PASSWORD
```

不要把密码写进脚本、配置或 Git。

### 2. 整理场景检测数据

```powershell
.\.venv\Scripts\python.exe .\prepare_scene_dataset.py
```

输出是官方 YOLOX 可直接读取的标准 COCO 目录：

```text
data/processed/scene_coco/
├── annotations/
│   ├── instances_train2017.json
│   ├── instances_val2017.json
│   └── instances_test2017.json
├── train2017/
├── val2017/
├── test2017/
└── dataset_audit.json
```

必须查看 `dataset_audit.json`。工具不会把 CribHD-C 无标注危险图片伪造成负样本，也不会把无法映射的类别静默改名。

### 3. 采集玩偶和最终设备画面

完整方案见 [VISION_COLLECTION_AND_FINETUNE_zh-CN.md](VISION_COLLECTION_AND_FINETUNE_zh-CN.md)。

从一段最终摄像头录像取 2 FPS、过滤相邻近重复帧：

```powershell
.\.venv\Scripts\python.exe .\capture_session.py `
  --video D:\captures\day_clear.mp4 `
  --session-id day_a_center_clear_01 `
  --doll-id doll_001 `
  --camera-id ra8_final_camera `
  --lighting day `
  --bed-set set_a `
  --target-fps 2
```

如果网站或 SD 卡已经保存 JPEG 文件：

```powershell
.\.venv\Scripts\python.exe .\capture_session.py `
  --image-dir D:\captures\camera_jpegs `
  --session-id night_a_edge_01 `
  --doll-id doll_001 `
  --camera-id ra8_final_camera `
  --lighting night_ir `
  --bed-set set_a
```

### 4. 标注

场景检测框建议用 CVAT：

1. 每个采集场次作为一个 task。
2. 标签只建立 `templates/scene_classes.txt` 中五类。
3. 导出 `COCO 1.0`。
4. 保持场次目录和场次 ID，不要把不同场次先混到一起。
5. 将导出目录放到 `data/raw/self_capture/coco/<split>/<session_id>/`。

两个分类模型复制 `templates/frame_annotations.csv` 到：

```text
data/raw/self_capture/frame_annotations.csv
```

填写头部框、婴儿框和两个标签。然后：

```powershell
.\.venv\Scripts\python.exe .\prepare_roi_datasets.py
```

#### 从历史红框图恢复到 SD 原图（prototype_v1）

`ml_training\data\打框数据集.zip` 的框被直接画进 JPG，不能直接训练。若 SD
原图已经复制到 `data\raw\self_capture_unboxed\<类别>\<同名图片>`，先运行：

```powershell
.\.venv\Scripts\python.exe .\recover_boxed_capture_dataset.py --force
.\.venv\Scripts\python.exe .\prepare_roi_datasets.py `
  --annotations .\data\raw\self_capture\prototype_v1\frame_annotations.csv `
  --output .\data\processed\prototype_v1_roi `
  --near-duplicate-distance 0 `
  --force
.\.venv\Scripts\python.exe .\prepare_scene_dataset.py `
  --self-root .\data\raw\self_capture\prototype_v1 `
  --force
```

恢复器要求目录和文件名一一对应，检查尺寸与感知哈希，并把坐标应用到没有红框
像素的 SD 原图；错配不会静默进入训练。报告保存在
`data\raw\self_capture\prototype_v1\recovery_report.json`。

这批历史目录只能训练试用原型：

- 遮挡只训练 `clear / covered / not_visible`；`baby_sheet` 内语义混杂，不能伪造
  `partial` 标签。
- 行为只训练 `quiet / active / edge_grab / unknown`；没有 `caregiver`。
- 划分来自同一批连续采集的带保护间隔时间段，不是独立采集场次，测试指标偏乐观。
- 分类 ROI 是婴儿/场景框，不是统一人工头部框，不能直接驱动高等级安全告警。

对应训练命令：

```powershell
.\.venv\Scripts\python.exe .\train_mobilenetv3.py `
  --task occlusion --classes clear covered not_visible `
  --data-root .\data\processed\prototype_v1_roi `
  --output .\artifacts\prototype_v1
.\.venv\Scripts\python.exe .\train_mobilenetv3.py `
  --task behavior --classes quiet active edge_grab unknown `
  --data-root .\data\processed\prototype_v1_roi `
  --output .\artifacts\prototype_v1
```

### 5. 训练两个分类模型

```powershell
.\.venv\Scripts\python.exe .\train_mobilenetv3.py --task occlusion
.\.venv\Scripts\python.exe .\train_mobilenetv3.py --task behavior
```

每个任务生成：

- 浮点 Keras 模型；
- strict INT8 输入/输出 TFLite；
- 浮点与 INT8 测试结果、混淆矩阵、量化参数和模型尺寸报告。

若测试目录尚无样本，脚本暂用验证集评估并在报告中标明；正式报告必须补独立测试场次。

### 6. 训练 YOLOX-Tiny

```powershell
.\setup_yolox.ps1
.\train_yolox.ps1 -Stage base -BatchSize 16
```

`base` 使用官方 YOLOX-Tiny 权重和当前 CribHD + SyRIP，允许
`caregiver` 暂时为空。采集、标注并重新运行 `prepare_scene_dataset.py --force`
后，再执行低学习率微调：

```powershell
.\train_yolox.ps1 -Stage finetune -BatchSize 16
```

`finetune` 会强制要求 train/val 都含自采图片和五个类别，默认从 base 最优权重开始；
base 与 finetune 产物分目录保存，不会互相覆盖。随后转换最终微调模型：

```powershell
.\convert_yolox_wsl.ps1 `
  -Model .\artifacts\scene\finetune\baby_yolox_tiny_224_finetune.onnx

.\.venv\Scripts\python.exe .\validate_tflite.py `
  --kind scene `
  --model .\artifacts\scene\baby_yolox_tiny_224_int8.tflite
```

`convert_yolox_int8.py` 固定沿用 Renesas 官方脚本和 YOLOX 训练管线的 OpenCV/BGR
张量顺序。板端 Renesas 参考预处理接收 RGB888 源图并交换为 BGR。集成最终摄像头
时必须做一次 PyTorch、TFLite、板端三方同图对齐测试；不能只看输出形状。

`export_yolox_onnx.py` 会把 YOLOX 的 Focus 与检测头布局改写为数学等价、但
onnx2tf 不会误判通道轴的导出图，并在每次导出时做 PyTorch 数值等价检查。
转换时仍必须运行 onnxsim，且转换器只优化临时副本并校验源 ONNX 哈希不变。

当前 Windows/Python 3.12 环境不能直接安装所需的 `onnxsim` 预编译轮子。因此训练和 ONNX 导出留在 Windows/CUDA，
ONNX→TFLite 固定在这台电脑已有的 WSL2 Ubuntu 中运行。`setup_conversion_wsl.ps1`
会安装独立环境，`convert_yolox_wsl.ps1` 自动转换 Windows/WSL 路径；不要求安装
Visual Studio C++ 编译器。

### 7. RUHMI / Ethos-U55

先从自定义 TFLite 读取实际量化参数，生成五类版本的预处理、后处理、元数据和类别表：

```powershell
.\.venv\Scripts\python.exe .\generate_scene_embedded_support.py `
  --model .\artifacts\scene\baby_yolox_tiny_224_int8.tflite
```

输出在 `artifacts/scene/embedded_support/`。Renesas 示例目录中的
`model_metadata.h` 是 COCO 80 类、85 输出通道，并且输出量化参数属于官方模型；
**不能原样用于本项目的 5 类、10 输出通道模型**。生成器还会强制检查输入量化必须是
参考预处理所支持的 `scale=1.0`、`zero_point=-128`，否则停止而不是生成错误固件。

先按 Renesas 官方说明安装 RUHMI/MERA 编译环境，然后：

```powershell
.\compile_ruhmi.ps1 `
  -Model .\artifacts\scene\baby_yolox_tiny_224_int8.tflite `
  -Target npu
```

两个分类模型也分别编译。本工作站也提供 WSL 入口。将 Renesas 官方 CPython
3.10 Linux MERA wheel 放到
`data/` 后，执行：

```powershell
.\setup_ruhmi_wsl.ps1
.\compile_ruhmi_wsl.ps1 `
  -Model .\artifacts\prototype_v1\occlusion\occlusion_mobilenetv3_small_192_int8.tflite `
  -Target npu `
  -Suffix occlusion
```

`setup_ruhmi_wsl.sh` 会把编译器隔离到 `~/.venvs/ra8p1_ruhmi_mera`，不会污染
TensorFlow 或 YOLOX 环境。YOLOX 模型超过内部存储预算时应显式加 `-External`，
并先确认板端 OSPI/外部 SDRAM 已实际配置。

每个模型都必须记录：

- RUHMI 编译是否所有主要算子下放 NPU；
- 模型常量、工作区和 tensor arena；
- 单模型和三模型串行调度峰值内存；
- 板端单图输出与电脑端最大误差；
- 实际摄像头到状态输出的端到端延迟。

## 当前硬件集成前置条件

当前工程的 `vision_preprocess_thread` 和 `vision_infer_thread` 仍处于挂起占位状态。
项目已确定继续使用 SCI3 VC0703 串口摄像头。当前 38400 波特率适合低速 JPEG
保存测试，但不能达到原规划的 2～4 FPS；应先按低帧率原型运行，后续只有在摄像头
与链路实测支持时才提高波特率和目标帧率。三模型部署前需要：

1. 在现有 camera IPC/SD 写入之外增加完整 JPEG 帧交付给 CPU0 的路径。
2. 跑通 JPEG 到 RGB888 或 RGB565 解码，再缩放至模型输入；当前工程没有 JPEG 解码器。
3. 配置并验证外部 SDRAM/OSPI；当前音频训练规格仍标记 `current_external_sdram: false`。
4. 实测三个 RUHMI 产物的总内存，不能按官方单模型演示值推测。
5. 保持 CPU1 负责传感器采集，CPU0 现有 `fusion_thread` 负责安全融合和上层控制。

在这些条件完成前，训练可以继续，但不能宣称视觉模型已在 RA8P1 实时闭环。
