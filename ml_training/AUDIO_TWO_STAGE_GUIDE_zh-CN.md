# RA8P1 两级哭声模型：数据、训练与量化指南

## 当前已经具备的基础

项目目录 `ml_training` 已具备以下完整链路：

```text
公开数据下载
→ 音频统一为 16 kHz / 单声道 / PCM16 WAV
→ 解码、静音、削波和时长审计
→ 按婴儿/设备实例/原始录音分组
→ 精确重复音频跨数据源合并分组
→ 70% / 15% / 15% 无泄漏划分
→ 1 秒窗口、250 ms 步长
→ 49 × 10 × 1 MFCC
→ 两个 DS-CNN 训练
→ 严格全 INT8 TFLite
→ Vela / Ethos-U55 内存和算子门禁
```

模型定义为：

| 模型 | 类别 |
| --- | --- |
| `cry_detection` | `non_cry`, `cry` |
| `cry_type` | 训练 5 类原因；低可信结果通过拒绝门限输出 `unknown` |

旧的 `cry`（49×40）规格被保留，只用于兼容此前产物；新的两级模型均使用
49×10 MFCC。

## 一、在 Windows 电脑上运行

进入：

```powershell
cd D:\Projects\e2studio\ra8p1\ml_training
```

检查或重建环境：

```powershell
powershell -ExecutionPolicy Bypass -File setup.ps1
```

下载并处理数据：

```powershell
powershell -ExecutionPolicy Bypass -File run_audio_pipeline.ps1
```

已经下载过时只重新扫描数据：

```powershell
powershell -File run_audio_pipeline.ps1 -SkipDownload
```

确认基础数据正确后，可以增加一份训练增强。增强只作用于训练集，会随机
平移、改变音量，并混入训练集中的非哭声背景：

```powershell
powershell -File run_audio_pipeline.ps1 -SkipDownload -AugmentCopies 1
```

不要在第一次基线训练前使用大量增强；先检查原始数据标签和混淆矩阵。

公开哭声的可播放 WAV 已整理到：

```text
data/ready_to_play/public_cry_wav/
├── train/
├── validation/
├── test/
├── all_public_cry.m3u
├── classification_v1_train.m3u
├── classification_v1_validation.m3u
├── classification_v1_test.m3u
└── index.csv
```

全部文件均为 16 kHz、单声道、PCM16 WAV。需要重新生成时运行：

```powershell
.\.venv\python.exe prepare_public_cry_library.py
```

不得把 `validation` 或 `test` 文件移入训练集；`untyped_cry` 只用于哭声检测。

## 二、必须先看的输出

处理完成后先检查：

```text
data/manifests/recordings.csv
data/manifests/dataset_summary.json
data/features/feature_summary.json
data/features/preprocessing_contract.json
```

其中：

- `recordings.csv` 记录每个原始文件、标签、质量、分组和最终划分。
- `dataset_summary.json` 记录各来源、类别和 train/validation/test 数量。
- `feature_summary.json` 记录各模型实际生成的窗口数。
- `preprocessing_contract.json` 是固件实现 MFCC 时的权威参数，包含两个
  模型共用的 10 个均值和标准差。

任何一个 `split_group` 都只能属于一个划分。脚本检测到分组泄漏会直接失败，
不会继续生成“看起来准确率很高、实际上泄漏”的训练集。

## 三、训练和 INT8 量化

同时训练两套模型：

```powershell
powershell -File train_audio_models.ps1 -Epochs 80
```

同时执行 Vela 编译门禁：

```powershell
powershell -File train_audio_models.ps1 -Epochs 80 -CompileVela
```

主要产物：

```text
artifacts/cry_detection/best.keras
artifacts/cry_detection/cry_detection_int8.tflite
artifacts/cry_detection/training_report.json
artifacts/cry_detection/int8_report.json

artifacts/cry_type/best.keras
artifacts/cry_type/cry_type_int8.tflite
artifacts/cry_type/training_report.json
artifacts/cry_type/int8_report.json
artifacts/temporal_policy_report.json
artifacts/deployment_validation/audio/report.json
```

量化门禁会检查：

- TFLite 输入、输出和中间张量均为整数；
- 模型没有超过 `model_specs.yaml` 的大小预算；
- 提供测试集时，INT8 相对浮点模型的准确率下降不超过 3%；
- Vela 没有意外 CPU 回退，SRAM 不超过预算。

本机有 RTX 4070，但 Windows 原生 TensorFlow 2.19 当前只识别 CPU。两套小型
DS-CNN 可以直接在 CPU 上训练；如果后续数据量显著扩大，再把同一目录放到
WSL2 + NVIDIA CUDA 环境训练，最终仍回 Windows 执行 INT8 和 Vela 验证。

## 四、你现在最需要做的：录制 RA8P1 实机数据

公开数据只能训练基础模型。最终模型必须加入板载 PDM 麦克风数据，尤其是
模型 1 的负样本。

先创建：

```text
data/raw/self_recorded/
├── metadata.csv
├── negative/
└── cry/
```

复制模板：

```powershell
Copy-Item templates\self_recorded_metadata.csv data\raw\self_recorded\metadata.csv
```

将网页测试页或 SD 卡保存的文件放进去，并在 `metadata.csv` 中逐条填写。
关键字段：

| 字段 | 填法 |
| --- | --- |
| `filename` | 相对 `self_recorded` 的文件路径 |
| `baby_id` | 匿名编号，如 `baby_001`；真实哭声必须填写 |
| `session_id` | 同一次录音会话使用同一编号 |
| `group_id` | 播放公开训练音频后用 RA8 重录时，复制原音频在 `recordings.csv` 中的 `group_id`；其他情况留空 |
| `detection_label` | `cry` 或 `non_cry` |
| `classification_label` | 六个类型之一；负样本留空 |
| `label_confidence` | `high`、`medium`、`low` 或 `unknown` |
| `device` | 固定写 `EK-RA8P1` |
| `distance_m` | 例如 `0.5`、`1.0`、`2.0` |
| `environment` | `quiet`、`fan`、`tv`、`talking` 等 |
| `source_partition` | 可固定为 `train`、`validation` 或 `test`；留空则自动划分 |

网页“数据集与算法实验”页中的音频标签也建议直接使用训练管线的标准值：

```text
模型 1：cry / non_cry
模型 2：hungry / belly_pain / discomfort / tired / burping / unknown
```

网页标签用于采集会话追踪；从 SD 卡或会话导出包取回 WAV 后，仍要在
`metadata.csv` 中确认 `detection_label` 和 `classification_label`。不要混用
`crying`、`non_crying` 等别名，以免后续人工转换时贴错标签。

同一个 `baby_id` 或 `session_id` 不得同时出现在训练集和测试集。脚本会把同一
分组强制放在一个划分中；如果手工指定发生冲突，会直接报错。

公开音频及其加噪版本、扬声器播放后的 RA8P1 重录版本必须填写同一个
`group_id`。应先划分原始录音，再做重录；绝不能让原始版进入训练集、重录版
进入测试集。

### 模型 1 优先录制清单

每种环境建议先录 10～20 分钟，并覆盖 0.5 m、1 m、2 m：

```text
安静房间
风扇和空调
成人说话
电视
音乐
咳嗽和笑声
脚步、关门
手机播放哭声
不同房间和不同朝向
```

“手机播放哭声”只能作为训练适配数据，不能代替真实环境最终测试。

### 模型 2 优先补充类别

当前公开数据中的 `belly_pain`、`burping` 和 `unknown` 明显不足。应优先补充：

```text
burping
belly_pain
unknown / 原因不确定
tired
```

标签必须来自监护人的当时记录，并保留 `label_confidence`。不要把低置信度
样本强制改成某个明确原因；放入 `unknown` 或暂不参与模型 2 更合理。

涉及真实婴儿时必须取得监护人明确同意，只保留匿名编号，不保存姓名、住址、
联系方式等身份信息。

## 五、多窗口决策

`audio_pipeline/decision.py` 已实现并经实录回放验证的当前规则：

```text
模型 1：cry 概率 ≥ 0.40，最近 8 个窗口至少命中 3 个
模型 1：确认后连续 12 个窗口没有新命中才释放，约 3 秒
模型 2：只累计被模型 1 判为哭声的最近 8 个窗口
模型 2：最高平均概率 < 0.75 → unknown
模型 2：第一名与第二名差值 < 0.25 → unknown
```

`data/selected/1` 和 `data/selected/2` 都是 RA8 每约 30 秒自动换文件的连续
录音。一个文件可能包含多条同标签播放样本；训练按窗口使用，划分与评估则按
完整播放会话分组，不能把滚动分卷数当作独立实验数。

这些阈值不能只看公开数据准确率确定。最终应在完整 RA8P1 实测会话上报告：

```text
哭声召回率
非哭声误报次数/小时
每个原因类别的 precision / recall / F1
混淆矩阵
浮点到 INT8 的准确率变化
板上端到端延迟
```

## 六、ICSD 的使用方式

ICSD 仍然需要你在 Hugging Face 页面人工同意条款并取得个人令牌。不要把令牌
写进脚本、项目或聊天记录。获批并下载后，把文件放到 `data/raw/ICSD`，再复制
并填写：

```text
templates/external_manifest.csv
→ data/raw/external_manifest.csv
```

ICSD 只用于模型 1 的哭声检测，不提供饥饿、腹痛等原因标签。

## 七、部署到 RA8P1 前的最后门禁

训练完成不等于可以直接烧录。固件接入时还必须：

1. Cortex-M85 使用与 `preprocessing_contract.json` 完全一致的窗函数、FFT、
   Mel 滤波、DCT、dB 截断、均值和标准差；512 点 FFT 中的 400 点周期
   Hann 窗采用居中补零。
2. 使用 TFLite 输入张量的 `scale` 和 `zero_point` 将 MFCC 转成 INT8。
3. 通过 RUHMI/Vela 生成 Ethos-U55 可执行模型。
4. 在 CPU0/CPU1 现有音频、SD、遥测任务同时运行时测峰值 SRAM 和实时性。
5. 模型输出只作为辅助状态；过热、遮挡、离床等安全规则仍以确定性逻辑为准。

哭声原因分类不能作为医疗诊断，网页和设备端均应显示“结果仅供辅助判断”。
