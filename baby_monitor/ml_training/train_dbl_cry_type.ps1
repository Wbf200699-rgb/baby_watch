param(
    [ValidateRange(1, 1000)][int]$Epochs = 80,
    [ValidateRange(1, 1024)][int]$BatchSize = 64,
    [switch]$DeployFirmware
)

$ErrorActionPreference = "Stop"
$python = if (Test-Path ".\.venv\python.exe") {
    (Resolve-Path ".\.venv\python.exe").Path
} elseif (Test-Path ".\.venv\Scripts\python.exe") {
    (Resolve-Path ".\.venv\Scripts\python.exe").Path
} else {
    throw "Training Python not found. Run setup.ps1 first"
}

$artifact = "artifacts\cry_type_mlp_dbl_tier2_v1"
$source = "infantcry_dbl_tier2"

& $python train.py cry_type_mlp `
    --train "data\features\cry_type_deploy_train.npz" `
    --val "data\features\cry_type_deploy_validation.npz" `
    --test "data\features\cry_type_deploy_test.npz" `
    --train-source $source `
    --validation-source $source `
    --test-source $source `
    --epochs $Epochs `
    --batch-size $BatchSize `
    --output $artifact
if ($LASTEXITCODE -ne 0) { throw "InfantCry-DBL Tier 2 training failed" }

$commonExport = @(
    "export_c_int8_type_mlp.py",
    "--model", "$artifact\best.keras",
    "--train", "data\features\cry_type_deploy_train.npz",
    "--train-source", $source,
    "--test-source", $source,
    "--decision-windows", "8",
    "--minimum-windows", "3",
    "--confidence-threshold", "0.65",
    "--margin-threshold", "0.15",
    "--output-header", "$artifact\audio_cry_type_model_data.h"
)

& $python @commonExport `
    --test "data\features\cry_type_deploy_validation.npz" `
    --output-report "$artifact\c_int8_validation_report.json"
if ($LASTEXITCODE -ne 0) { throw "Validation-set C INT8 export failed" }

& $python @commonExport `
    --test "data\features\cry_type_deploy_test.npz" `
    --output-report "$artifact\c_int8_report.json"
if ($LASTEXITCODE -ne 0) { throw "Test-set C INT8 export failed" }

if ($DeployFirmware) {
    Copy-Item `
        "$artifact\audio_cry_type_model_data.h" `
        "..\Multicore_top_CPU0\src\thread_code\audio_cry_type_model_data.h" `
        -Force
    Write-Host "Updated the CPU0 cry-type model header."
} else {
    Write-Host "Firmware was not changed. Re-run with -DeployFirmware after reviewing both reports."
}

Write-Host "Model: $artifact"
Write-Host "Review c_int8_validation_report.json and c_int8_report.json."
