$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Vendor = Join-Path $Root "vendor"
$YoloXCommit = "6ddff4824372906469a7fae2dc3206c7aa4bbaee"
$RuhmiCommit = "c4e393c8ae1d54c78bf38efffbfa0f0213113ff4"
New-Item -ItemType Directory -Force -Path $Vendor | Out-Null

$YoloX = Join-Path $Vendor "YOLOX"
if (-not (Test-Path (Join-Path $YoloX ".git"))) {
    git clone --depth 1 https://github.com/Megvii-BaseDetection/YOLOX.git $YoloX
}
git -C $YoloX fetch --depth 1 origin $YoloXCommit
git -C $YoloX checkout --detach $YoloXCommit

$Ruhmi = Join-Path $Vendor "ruhmi-model-zoo"
if (-not (Test-Path (Join-Path $Ruhmi ".git"))) {
    git clone --depth 1 --filter=blob:none --sparse `
        https://github.com/renesas/ruhmi-model-zoo.git $Ruhmi
    if (-not (Test-Path (Join-Path $Ruhmi ".git"))) {
        throw "RUHMI model zoo clone did not complete; sparse-checkout was not run."
    }
    git -C $Ruhmi sparse-checkout set `
        ruhmi_tools `
        vision/object_detection/yolox_tiny `
        vision/image_classification/mobilenetv3
}
git -C $Ruhmi fetch --depth 1 origin $RuhmiCommit
git -C $Ruhmi checkout --detach $RuhmiCommit

$Versions = @{
    yolox = (git -C $YoloX rev-parse HEAD)
    ruhmi_model_zoo = (git -C $Ruhmi rev-parse HEAD)
    captured_at = (Get-Date).ToUniversalTime().ToString("o")
}
$Versions | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Vendor "versions.json")
Write-Host "Official external repositories are ready under $Vendor (Git-ignored)."
