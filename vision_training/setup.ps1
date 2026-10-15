param(
    [string]$Python = "py -3.12"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$PythonParts = $Python -split " "
if ($PythonParts.Count -eq 1) {
    & $PythonParts[0] -m venv .venv
} else {
    & $PythonParts[0] $PythonParts[1] -m venv .venv
}
if ($LASTEXITCODE -ne 0) {
    throw "Python 3.12 is required for this workstation setup. Install it or pass -Python."
}

& .\.venv\Scripts\python.exe -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw "Failed to upgrade pip in .venv."
}
& .\.venv\Scripts\python.exe -m pip install -r requirements.txt
if ($LASTEXITCODE -ne 0) {
    throw "Failed to install vision requirements."
}
& .\.venv\Scripts\python.exe -m pytest tests -q
if ($LASTEXITCODE -ne 0) {
    throw "Vision pipeline tests failed."
}

Write-Host "Vision data and MobileNetV3 environment ready: $Root\.venv"
