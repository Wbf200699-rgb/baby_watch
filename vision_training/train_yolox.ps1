param(
    [ValidateSet("base", "finetune")][string]$Stage = "base",
    [int]$BatchSize = 16,
    [int]$Devices = 1,
    [int]$Epochs = 0,
    [switch]$NoFp16,
    [switch]$Occupy,
    [switch]$Resume,
    [string]$Checkpoint = "",
    [string]$DataRoot = "",
    [string]$RunName = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Python = Join-Path $Root ".venv_yolox\Scripts\python.exe"
$YoloX = Join-Path $Root "vendor\YOLOX"
if (-not $DataRoot) {
    $DataRoot = Join-Path $Root "data\processed\scene_coco"
}
$Data = [System.IO.Path]::GetFullPath($DataRoot)
$RunRoot = Join-Path $Root "artifacts\scene\runs"
$StageOutput = Join-Path $Root "artifacts\scene\$Stage"
$Exp = Join-Path $Root "yolox_baby_tiny_exp.py"

if (-not (Test-Path $Python)) {
    throw "Run .\setup_yolox.ps1 first."
}
# Invoking a venv Python by absolute path does not activate the venv, so console
# tools installed by packages (notably ninja.exe for YOLOX COCO evaluation)
# would otherwise be invisible to torch's extension loader.
$env:PATH = "$(Split-Path -Parent $Python);$env:PATH"
if (-not (Test-Path (Join-Path $Data "annotations\instances_train2017.json"))) {
    throw "Run prepare_scene_dataset.py first."
}

$env:BABY_VISION_COCO_ROOT = $Data
$env:BABY_VISION_OUTPUT_DIR = $RunRoot
if (-not $RunName) {
    $RunName = "baby_vision_yolox_tiny_224_$Stage"
}
$env:BABY_VISION_EXP_NAME = $RunName
if ($Stage -eq "base") {
    if ($Epochs -le 0) { $Epochs = 120 }
    $env:BABY_VISION_WARMUP_EPOCHS = "5"
    $env:BABY_VISION_NO_AUG_EPOCHS = "15"
    $env:BABY_VISION_BASIC_LR_PER_IMG = "0.00015625"
} else {
    if ($Epochs -le 0) { $Epochs = 50 }
    $env:BABY_VISION_WARMUP_EPOCHS = "2"
    $env:BABY_VISION_NO_AUG_EPOCHS = "10"
    $env:BABY_VISION_BASIC_LR_PER_IMG = "0.00003125"
}
$env:BABY_VISION_MAX_EPOCH = "$Epochs"

& $Python (Join-Path $Root "scene_dataset_preflight.py") `
    --data-root $Data --stage $Stage
if ($LASTEXITCODE -ne 0) {
    throw "Scene dataset preflight failed for stage: $Stage"
}

if ((-not $Resume) -and (-not $Checkpoint)) {
    if ($Stage -eq "base") {
        $PretrainedDir = Join-Path $Root "artifacts\pretrained"
        New-Item -ItemType Directory -Force -Path $PretrainedDir | Out-Null
        $Checkpoint = Join-Path $PretrainedDir "yolox_tiny.pth"
        if (-not (Test-Path $Checkpoint)) {
            & curl.exe --fail --location --retry 5 `
                --output $Checkpoint `
                "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_tiny.pth"
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to download the official YOLOX-Tiny pretrained checkpoint."
            }
        }
    } else {
        $Checkpoint = Join-Path $RunRoot "baby_vision_yolox_tiny_224_base\best_ckpt.pth"
    }
}
if ((-not $Resume) -and (-not (Test-Path $Checkpoint))) {
    throw "Checkpoint was not found: $Checkpoint"
}
$Arguments = @(
    (Join-Path $YoloX "tools\train.py"),
    "-f", $Exp,
    "-d", "$Devices",
    "-b", "$BatchSize"
)
if ($Occupy) {
    $Arguments += "-o"
}
if (-not $NoFp16) {
    $Arguments += "--fp16"
}
if ($Resume) {
    $Arguments += "--resume"
    if ($Checkpoint) {
        if (-not (Test-Path $Checkpoint)) {
            throw "Resume checkpoint was not found: $Checkpoint"
        }
        $Arguments += @("-c", $Checkpoint)
    }
} else {
    $Arguments += @("-c", $Checkpoint)
}
& $Python @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "YOLOX training failed."
}

$Best = Join-Path $RunRoot "$($env:BABY_VISION_EXP_NAME)\best_ckpt.pth"
if (-not (Test-Path $Best)) {
    throw "Training finished but best_ckpt.pth was not found: $Best"
}
New-Item -ItemType Directory -Force -Path $StageOutput | Out-Null
$Onnx = Join-Path $StageOutput "baby_yolox_tiny_224_$Stage.onnx"
& $Python (Join-Path $Root "export_yolox_onnx.py") `
    --checkpoint $Best --output $Onnx --opset 18
if ($LASTEXITCODE -ne 0) {
    throw "ONNX export failed."
}
Write-Host "Training and ONNX export complete: $Onnx"
Write-Host "Next: .\convert_yolox_wsl.ps1 -Model `"$Onnx`""
