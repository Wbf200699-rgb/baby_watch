param(
    [ValidateRange(1, 1000)][int]$Epochs = 80,
    [ValidateRange(1, 1024)][int]$BatchSize = 64,
    [switch]$CompileVela
)

$ErrorActionPreference = "Stop"
$python = if (Test-Path ".\.venv\python.exe") {
    (Resolve-Path ".\.venv\python.exe").Path
} elseif (Test-Path ".\.venv\Scripts\python.exe") {
    (Resolve-Path ".\.venv\Scripts\python.exe").Path
} else {
    throw "Training Python not found. Run setup.ps1 first"
}

foreach ($model in @("cry_detection", "cry_type")) {
    & $python train.py $model `
        --train "data\features\${model}_train.npz" `
        --val "data\features\${model}_validation.npz" `
        --test "data\features\${model}_test.npz" `
        --epochs $Epochs `
        --batch-size $BatchSize
    if ($LASTEXITCODE -ne 0) { throw "$model training failed" }

    & $python export_int8.py $model --test "data\features\${model}_test.npz"
    if ($LASTEXITCODE -ne 0) { throw "$model INT8 export failed" }

}

if ($CompileVela) {
    & $python validate_deployment.py cry_detection cry_type `
        --output-dir "artifacts\deployment_validation\audio"
    if ($LASTEXITCODE -ne 0) { throw "Audio-model Vela validation failed" }
}

& $python evaluate_temporal_policy.py `
    --detection-model "artifacts\cry_detection\best.keras" `
    --detection-data "data\features\cry_detection_test.npz" `
    --type-model "artifacts\cry_type\best.keras" `
    --type-data "data\features\cry_type_test.npz" `
    --output "artifacts\temporal_policy_report.json"
if ($LASTEXITCODE -ne 0) { throw "Temporal policy evaluation failed" }

Write-Host "Both audio models passed training and strict INT8 export."
