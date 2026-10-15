param(
    [string]$Python = "py -3.12"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $Root "setup_external_repos.ps1")

$PythonParts = $Python -split " "
if ($PythonParts.Count -eq 1) {
    & $PythonParts[0] -m venv (Join-Path $Root ".venv_yolox")
} else {
    & $PythonParts[0] $PythonParts[1] -m venv (Join-Path $Root ".venv_yolox")
}
$VenvPython = Join-Path $Root ".venv_yolox\Scripts\python.exe"
& $VenvPython -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw "Failed to upgrade pip in .venv_yolox."
}
& $VenvPython -m pip install `
    torch==2.13.0 `
    torchvision==0.28.0 `
    --index-url https://download.pytorch.org/whl/cu130
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install the official PyTorch CUDA 13.0 wheels."
}
& $VenvPython -m pip install `
    numpy==1.26.4 `
    opencv-python==4.11.0.86 `
    loguru==0.7.3 `
    tqdm==4.67.3 `
    thop==0.1.1.post2209072238 `
    ninja==1.13.0 `
    tabulate==0.9.0 `
    psutil==5.9.5 `
    tensorboard==2.19.0 `
    pycocotools==2.0.11 `
    open_clip_torch==3.2.0
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install YOLOX runtime dependencies."
}
& $VenvPython -m pip install --no-deps -e (Join-Path $Root "vendor\YOLOX")
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install the pinned official YOLOX source."
}
& $VenvPython -m pip install `
    onnx==1.19.1 `
    onnxruntime==1.23.0
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install ONNX dependencies."
}
& $VenvPython -c "import onnx, onnxruntime, torch, yolox; print('YOLOX/CUDA:', torch.__version__, torch.cuda.is_available())"
if ($LASTEXITCODE -ne 0) {
    throw "YOLOX environment import smoke test failed."
}

Write-Host "YOLOX training/ONNX environment ready: $Root\.venv_yolox"
Write-Host "Run .\setup_conversion_wsl.ps1 once for the supported onnxsim/onnx2tf environment."
