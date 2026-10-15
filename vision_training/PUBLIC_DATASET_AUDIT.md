# 公开视觉数据准备审计

审计日期：2026-07-25。原始压缩包和处理后图片不进入 Git；本文件记录这台训练工作站上已验证的数据版本与第一次整理结果。

## 原始文件

| 数据集 | 文件大小 | SHA-256 |
| --- | ---: | --- |
| CribHD.zip | 640,664,878 bytes | `c771c76755ebc39f090ad50d83a10c1e5cc0e8b979c21c75fcafc57f319e4f9f` |
| SyRIP.zip | 327,912,103 bytes | `1d13e816375988be9bbdab8bf5be6562a5b9bfa83684d57ccbfec9dd619e1692` |

下载器已把上述大小和哈希作为完整性校验。若官方以后替换同名压缩包，应先核实内容与许可，再有意更新登记值，不能绕过校验。

## 统一 COCO 结果

目标类别顺序固定为：

```text
infant, head_face, blanket, soft_object, caregiver
```

第一次运行 `prepare_scene_dataset.py --force` 的结果：

| 划分 | 图片 | 标注框 | 类别计数 |
| --- | ---: | ---: | --- |
| train | 2022 | 3880 | infant 1108；head_face 1108；blanket 562；soft_object 1102 |
| val | 660 | 1293 | infant 592；head_face 592；blanket 48；soft_object 61 |
| test | 20 | 40 | blanket 13；soft_object 27 |

来源审计：

- SyRIP：1700/1700 张均找到，1700 个 `infant` 框；另从面部关键点派生 1700 个弱标注 `head_face` 框。
- SyRIP 官方 train/validate 中有 24 个规范化后相同的来源组跨划分。整理器把这些组整体隔离到 val，共移动 92 张 train 图片；最终不存在相同视频/URL 来源同时进入 train 和 val。
- CribHD：1500 条图片记录中保留 1002 张含目标类别的图片、1813 个目标框。
- CribHD 的 1211 个 `hard toy` 框被有意排除；项目的 `soft_object` 不应错误包含硬玩具。
- CribHD 官方 ZIP 缺少 COCO train 中一条超长文件名对应的图片。该记录是一个 `blanket` 框，下载包内按完整文件名和 Roboflow 哈希均找不到，因此审计明确记录为 1 张缺图，没有伪造或替代样本。
- `caregiver` 必须由最终摄像头的自采场次补齐。
- 当前公开 test 没有 `infant/head_face`；正式测试必须加入按场次隔离的自采测试集，不能用训练或验证帧冒充。

处理后机器可读报告位于被 Git 忽略的：

```text
data/processed/scene_coco/dataset_audit.json
```

CribHD 与 SyRIP 官方仓库均有限制性使用说明；当前仅按研究、课程和原型用途准备，商业用途必须重新取得或确认授权。
