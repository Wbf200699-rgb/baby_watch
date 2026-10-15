param(
    [Parameter(Mandatory = $true)][string]$Model,
    [string]$Output = "",
    [string]$CalibrationRoot = "",
    [int]$Samples = 256,
    [string]$Distro = "Ubuntu",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ModelFull = (Resolve-Path $Model).Path
if (-not $Output) {
    $Output = Join-Path $Root "artifacts\scene\baby_yolox_tiny_224_int8.tflite"
}
if (-not [System.IO.Path]::IsPathRooted($Output)) {
    $Output = Join-Path (Get-Location).Path $Output
}
$OutputFull = [System.IO.Path]::GetFullPath($Output)
if (-not $CalibrationRoot) {
    $CalibrationRoot = Join-Path $Root "data\processed\scene_coco\train2017"
}
$CalibrationFull = (Resolve-Path $CalibrationRoot).Path

$RootWsl = (& wsl.exe -d $Distro -- wslpath -a $Root.Replace('\', '/')).Trim()
$ModelWsl = (& wsl.exe -d $Distro -- wslpath -a $ModelFull.Replace('\', '/')).Trim()
$OutputWsl = (& wsl.exe -d $Distro -- wslpath -a $OutputFull.Replace('\', '/')).Trim()
$CalibrationWsl = (& wsl.exe -d $Distro -- wslpath -a $CalibrationFull.Replace('\', '/')).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Could not map one or more paths into WSL."
}

$Arguments = @(
    "$RootWsl/convert_yolox_wsl.sh",
    "--onnx", $ModelWsl,
    "--calibration-dir", $CalibrationWsl,
    "--output", $OutputWsl,
    "--samples", "$Samples"
)
if ($Force) {
    $Arguments += "--force"
}
& wsl.exe -d $Distro -- bash @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "YOLOX INT8 conversion failed in WSL."
}
Write-Host "YOLOX strict INT8 output: $OutputFull"
