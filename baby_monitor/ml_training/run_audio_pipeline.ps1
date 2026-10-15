param(
    [switch]$SkipDownload,
    [switch]$ForceStandardize,
    [ValidateRange(0, 8)][int]$AugmentCopies = 0
)

$ErrorActionPreference = "Stop"
$python = if (Test-Path ".\.venv\python.exe") {
    (Resolve-Path ".\.venv\python.exe").Path
} elseif (Test-Path ".\.venv\Scripts\python.exe") {
    (Resolve-Path ".\.venv\Scripts\python.exe").Path
} else {
    throw "Training Python not found. Run: powershell -ExecutionPolicy Bypass -File setup.ps1"
}

if (-not $SkipDownload) {
    & $python download_audio_datasets.py
    if ($LASTEXITCODE -ne 0) { throw "Dataset download failed" }
}

$arguments = @("prepare_audio_data.py", "all", "--augment-copies", $AugmentCopies)
if ($ForceStandardize) { $arguments += "--force" }
& $python @arguments
if ($LASTEXITCODE -ne 0) { throw "Audio preparation failed" }

Write-Host "Prepared NPZ files are in data\features."
Write-Host "Review data\manifests\dataset_summary.json before training."
