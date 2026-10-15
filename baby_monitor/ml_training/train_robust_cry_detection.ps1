param(
    [ValidateRange(1, 500)][int]$Epochs = 80,
    [ValidateRange(1, 1024)][int]$BatchSize = 128,
    [switch]$RebuildFeatures,
    [switch]$DeployFirmware
)

$ErrorActionPreference = "Stop"
$python = if (Test-Path ".\.venv\python.exe") {
    (Resolve-Path ".\.venv\python.exe").Path
} elseif (Test-Path ".\.venv\Scripts\python.exe") {
    (Resolve-Path ".\.venv\Scripts\python.exe").Path
} else {
    throw "Training Python not found. Run setup.ps1 first."
}

$artifactDir = "artifacts\cry_detection_mlp_hardneg_v2b"
$model = "$artifactDir\best.keras"
$train = "data\features\cry_detection_train.npz"
$validation = "data\features\cry_detection_validation.npz"
$test = "data\features\cry_detection_test.npz"
$contract = "data\features\preprocessing_contract.json"

function Invoke-CheckedPython {
    & $python @args
    if ($LASTEXITCODE -ne 0) {
        throw "Python command failed: $($args -join ' ')"
    }
}

if ($RebuildFeatures) {
    Invoke-CheckedPython download_audio_datasets.py
    Invoke-CheckedPython prepare_audio_data.py all `
        --augment-copies 1 `
        --device-chain-copies 1 `
        --short-cry-copies 1
}

Invoke-CheckedPython train.py cry_detection_mlp `
    --train $train `
    --val $validation `
    --test $test `
    --epochs $Epochs `
    --batch-size $BatchSize `
    --source-weight device_selected_ra8p1=3.0 `
    --source-weight device_selected_ra8p1_type=2.0 `
    --source-weight tut_rare_events_babycry=2.0 `
    --source-weight ravdess_speech=2.0 `
    --output $artifactDir

Invoke-CheckedPython evaluate_detection_thresholds.py `
    --model $model --data $validation `
    --thresholds 0.50,0.60,0.65,0.70,0.75 `
    --vote-window 8 --required-hits 3 `
    --output "$artifactDir\validation_thresholds.json"
Invoke-CheckedPython evaluate_detection_thresholds.py `
    --model $model --data $test `
    --source device_selected_ra8p1 `
    --thresholds 0.50,0.60,0.65,0.70,0.75 `
    --vote-window 8 --required-hits 3 `
    --output "$artifactDir\device_thresholds.json"
Invoke-CheckedPython evaluate_detection_thresholds.py `
    --model $model --data $test `
    --source ravdess_speech `
    --thresholds 0.50,0.60,0.65,0.70,0.75 `
    --vote-window 8 --required-hits 3 `
    --output "$artifactDir\ravdess_thresholds.json"

$generatedHeader = "$artifactDir\audio_cry_model_data.h"
Invoke-CheckedPython export_c_int8_mlp.py `
    --model $model `
    --train $train `
    --test $test `
    --config audio_two_stage.yaml `
    --contract $contract `
    --threshold 0.65 `
    --consecutive 3 `
    --output-header $generatedHeader `
    --output-report "$artifactDir\c_int8_report.json"

if ($DeployFirmware) {
    $firmwareHeader = "..\Multicore_top_CPU0\src\thread_code\audio_cry_model_data.h"
    Copy-Item -LiteralPath $generatedHeader -Destination $firmwareHeader -Force
    Write-Host "Deployed model header to $firmwareHeader"
}

Write-Host "Robust cry detector artifacts: $artifactDir"
