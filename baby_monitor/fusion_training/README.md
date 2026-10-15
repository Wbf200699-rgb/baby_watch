# Fusion calibration

`calibrate_fusion.py` consumes the nested session export produced by the
server data-collection page. It derives an explainable per-modality profile;
it does not treat independently recorded pressure, thermal, and bed-
temperature samples as synchronized observations.

Run from the repository root:

```powershell
python fusion_training\calibrate_fusion.py `
  "D:\Projects\e2studio\ra8p1\数据集2.0.zip" `
  --header Multicore_top_CPU0\src\thread_code\fusion_calibration_generated.h `
  --profile fusion_training\artifacts\fusion_dataset_2_0_profile.json `
  --report docs\FUSION_DATASET_2_0_REPORT_zh-CN.md
```

The generated header is a CPU0 build input. The JSON profile and report retain
the source archive SHA-256 and replay metrics. Re-run calibration after moving
the pressure mat, changing the bed/camera geometry, or changing the heater.
The 25 MB capture archive is intentionally not duplicated into this repository;
the checked-in SHA-256 makes the generated profile traceable to the original.

The thermal matrix contract remains RA8M/1: 64 rows by 80 columns, row-major
little-endian `int16`, in 0.1 degree Celsius units.
