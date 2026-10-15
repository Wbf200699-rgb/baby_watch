param(
    [Parameter(Mandatory = $true)][string]$Model,
    [string]$OutputDir = "artifacts/vela"
)

$ErrorActionPreference = "Stop"
$modelPath = "artifacts/$Model/${Model}_int8.tflite"
if (-not (Test-Path $modelPath)) { throw "Model not found: $modelPath" }
New-Item -ItemType Directory -Force $OutputDir | Out-Null

vela $modelPath `
    --accelerator-config ethos-u55-256 `
    --optimise Performance `
    --memory-mode Shared_Sram `
    --system-config Ethos_U55_High_End_Embedded `
    --output-dir $OutputDir `
    --show-cpu-operations

if ($LASTEXITCODE -ne 0) { throw "Vela compilation failed" }
