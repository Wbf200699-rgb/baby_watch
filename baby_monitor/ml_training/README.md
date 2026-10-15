# RA8P1 model training workspace

This folder trains models whose deployment target is the R7KA8P1KFLCAC:
Cortex-M85 plus Ethos-U55-256. Training cost is unrestricted; deployment is
strictly full-integer INT8 TensorFlow Lite, followed by RUHMI/Vela compilation.

The baby-cry workflow is now implemented as two independent models:

1. `cry_detection`: `non_cry` / `cry`.
2. `cry_type`: `hungry`, `belly_pain`, `discomfort`, `tired`, `burping`,
   `unknown`.

Both consume the same normalized `49x10x1` MFCC tensor. See
[AUDIO_TWO_STAGE_GUIDE_zh-CN.md](AUDIO_TWO_STAGE_GUIDE_zh-CN.md) for the
complete Chinese workflow and the actions still required from the device
owner.

## Selected models

| Task | Input | Network | Deployment reason |
| --- | --- | --- | --- |
| Cry detection | 49x10 MFCC, 1 s | DS-CNN | Low-latency first-stage detector |
| Cry type | 49x10 MFCC, 1 s | DS-CNN | Shares the same Cortex-M85 feature buffer |
| Pressure pose | 16x16 normalized pressure | three-layer Tiny-CNN | Preserves spatial load shape with a very small arena |
| Thermal state | 64x80 normalized temperature | DS-CNN | Handles the native sensor frame without resize and stays on the NPU |
| Vision state | 160x160 RGB | MobileNetV3-Small 0.5 | Supported operator family and useful accuracy/size tradeoff |
| Multimodal fusion | 24 features | 32x24 MLP | Tiny, deterministic final classifier; safety rules remain authoritative |

The vision model is intentionally gated as requiring external memory. The
current CPU0 configuration has SDRAM disabled, so do not integrate it until
SDRAM/OSPI is configured and its Vela arena report fits. Cry, pressure, thermal,
and fusion are sized for the on-chip-memory path, but the final RUHMI memory
report is still the release gate because firmware and model share SRAM.

## Environment

Use Python 3.11 on Windows or Linux. From this directory:

```powershell
powershell -ExecutionPolicy Bypass -File setup.ps1
.\.venv\Scripts\Activate.ps1
```

The existing `.venv` may be either a Conda prefix (`.venv\python.exe`) or a
normal venv (`.venv\Scripts\python.exe`); the PowerShell scripts support both.
For GPU/WSL training, create the same environment from `requirements.txt`.
Vela compilation does not require a GPU and should also be repeated on the
deployment workstation.

## Prepare the audio datasets

The pipeline is idempotent. It downloads only public, non-gated sources,
standardizes audio with FFmpeg, audits quality, groups related recordings
before splitting, and emits NPZ files:

```powershell
powershell -ExecutionPolicy Bypass -File run_audio_pipeline.ps1
```

The five-way reason model now also uses InfantCry-DBL v1. Its five DBL cues
map directly to the firmware order:
`neh -> hungry`, `eairh -> belly_pain`, `heh -> discomfort`,
`owh -> tired`, and `eh -> burping`. Tier 1 is training-only; Tier 2 is
grouped by anonymised clip/infant ID before splitting.

The deployed five-way classifier is intentionally trained and evaluated only
on the more varied Tier 2 subset. This avoids mixing incompatible label
definitions from older cause-labelled corpora. Reproduce its training, C INT8
export and recording-level reject-policy reports with:

```powershell
powershell -ExecutionPolicy Bypass -File train_dbl_cry_type.ps1
```

Add `-DeployFirmware` only after reviewing both generated reports. It copies
the accepted header into CPU0; it does not alter the first-stage cry detector.

To add RA8P1 recordings, copy
`templates/self_recorded_metadata.csv` to
`data/raw/self_recorded/metadata.csv`, add the WAV files referenced by it, and
run:

```powershell
powershell -File run_audio_pipeline.ps1 -SkipDownload
```

To rebuild the human-browsable public cry library and M3U playlists:

```powershell
.\.venv\python.exe prepare_public_cry_library.py
```

It writes 16 kHz mono PCM16 WAV files under
`data/ready_to_play/public_cry_wav`, grouped by the already-fixed
train/validation/test split and cry-type label. `index.csv` retains the
original source, group and license for every copied file. For a clean board
test of the deployed five-way model, play only
`dbl_tier2_test_<label>.m3u`; the train and validation lists have already been
seen during model development.

## Dataset contract

Each trainer consumes two NPZ files containing `x` float32 and `y` integer
class IDs. Shapes and class order are authoritative in `model_specs.yaml`.
Split by baby/session, not random windows, to prevent leakage. Keep raw WAV,
sensor frames and labels outside Git under `data/` or on the SD archive.

Audio preprocessing is frozen in `audio_two_stage.yaml`: 16 kHz mono, one
second windows with a 250 ms step, 25 ms Hann windows, 20 ms frame step,
512-point FFT, 40 Mel bands, and the first 10 MFCC coefficients. This produces
49 time frames. The generated `data/features/preprocessing_contract.json`
contains the shared per-coefficient training mean and standard deviation that
must be copied exactly into Cortex-M85 firmware. Pressure
uses empty-bed baseline subtraction followed by a fixed robust scale. Thermal
uses temperatures clipped to the training range and a fixed affine scale.

## Train and export

```powershell
powershell -File train_audio_models.ps1 -Epochs 80 -CompileVela
```

For an individual audio model:

```powershell
python train.py cry_detection `
  --train data/features/cry_detection_train.npz `
  --val data/features/cry_detection_validation.npz `
  --test data/features/cry_detection_test.npz
python export_int8.py cry_detection --test data/features/cry_detection_test.npz
python validate_deployment.py cry_detection
```

Repeat the generic flow for `pressure`, `thermal`, `vision`, and `fusion`.
`export_int8.py`
rejects float tensors and oversized model files. Vela must report no unexpected
CPU operators; review its peak SRAM estimate against `max_arena_bytes` before
firmware integration. Accuracy, recall, false alarms per hour, INT8 accuracy
drop, Vela CPU fallbacks, arena size and measured board latency are mandatory
release metrics.

`train_audio_models.ps1` also writes
`artifacts/temporal_policy_report.json`. That report groups windows back into
complete recordings, applies the configured three-window cry trigger and
multi-window reason vote, and reports recording-level false positives and
abstentions. With `-CompileVela`, the combined operator and SRAM gate for both
audio networks is written to
`artifacts/deployment_validation/audio/report.json`, so one model cannot
overwrite the other model's report.

The RA8P1 device-recording model, firmware telemetry fields, measured results,
and RFP test procedure are documented in
[`DEVICE_CRY_MODEL_zh-CN.md`](DEVICE_CRY_MODEL_zh-CN.md).

## Safety boundary

Model confidence augments the existing fusion state. It must not replace the
rule-based overheat, edge, out-of-bed, stale-sensor, ACK/retry, or fault paths.
Use temporal debounce and tune alarm thresholds on held-out complete sessions.
The inferred cry reason is an assistive estimate, not a medical diagnosis.
