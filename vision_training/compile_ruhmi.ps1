param(
    [Parameter(Mandatory = $true)][string]$Model,
    [ValidateSet("npu", "cpu")][string]$Target = "npu",
    [string]$Output = "",
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Ruhmi = Join-Path $Root "vendor\ruhmi-model-zoo"
if (-not (Test-Path (Join-Path $Ruhmi "ruhmi_tools\mcu_compile.py"))) {
    throw "Run .\setup_external_repos.ps1 and configure the Renesas RUHMI/MERA environment."
}

$ModelPath = (Resolve-Path $Model).Path
if (-not $Output) {
    $Output = Join-Path $Root "artifacts\ruhmi_$Target"
}
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$OutputPath = (Resolve-Path $Output).Path
$ConfigPath = Join-Path $Root "artifacts\ruhmi_compile_$Target.yaml"

@"
model_path: "$($ModelPath.Replace('\', '/'))"
output_dir: "$($OutputPath.Replace('\', '/'))"
target: $Target
quantize: false
"@ | Set-Content -Encoding UTF8 $ConfigPath

if (-not $Python) {
    $Python = Join-Path $Ruhmi ".mera_venv\Scripts\python.exe"
}
if (-not (Test-Path $Python)) {
    throw "RUHMI compiler Python was not found: $Python. Pass -Python to select one."
}
& $Python (Join-Path $Ruhmi "ruhmi_tools\mcu_compile.py") $ConfigPath
if ($LASTEXITCODE -ne 0) {
    throw "RUHMI compilation failed."
}
Write-Host "RUHMI $Target output: $OutputPath"
