$ErrorActionPreference = "Stop"

function Get-TrainingPython {
    if (Test-Path ".\.venv\python.exe") {
        return (Resolve-Path ".\.venv\python.exe").Path
    }
    if (Test-Path ".\.venv\Scripts\python.exe") {
        return (Resolve-Path ".\.venv\Scripts\python.exe").Path
    }
    if (Test-Path ".\.venv") {
        throw ".venv exists but has no Python executable; rename or remove only that broken environment"
    }
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    if ($launcher) {
        & $launcher.Source -3.11 -m venv .venv
    } else {
        $python = Get-Command python -ErrorAction Stop
        & $python.Source -m venv .venv
    }
    if ($LASTEXITCODE -ne 0) { throw "Python environment creation failed" }
    return (Resolve-Path ".\.venv\Scripts\python.exe").Path
}

$python = Get-TrainingPython
& $python -m pip install --upgrade pip wheel
if ($LASTEXITCODE -ne 0) { throw "pip bootstrap failed" }
& $python -m pip install -r requirements.txt
if ($LASTEXITCODE -ne 0) { throw "dependency installation failed" }
& $python -m pytest tests -q
if ($LASTEXITCODE -ne 0) { throw "audio pipeline tests failed" }
& $python smoke_test.py --model cry_detection
& $python smoke_test.py --model cry_type
Write-Host "Environment ready. Python: $python"
