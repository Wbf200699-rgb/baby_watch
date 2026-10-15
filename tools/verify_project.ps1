param(
    [string]$E2StudioRoot = "C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$toolchain = Join-Path $E2StudioRoot "toolchains\llvm_arm\ATfE-21.1.1-Windows-x86_64\bin"
$makeTools = Join-Path $E2StudioRoot "eclipse\plugins\com.renesas.ide.exttools.gnumake.win32.x86_64_4.3.1.v20240909-0854\mk"

if (-not (Test-Path (Join-Path $toolchain "clang.exe"))) {
    throw "RA LLVM toolchain not found under $toolchain"
}
if (-not (Test-Path (Join-Path $makeTools "make.exe"))) {
    throw "GNU make not found under $makeTools"
}

$env:PATH = "$toolchain;$makeTools;$env:PATH"

function Invoke-FirmwareBuild([string]$Project) {
    $buildDir = Join-Path $repo "$Project\Debug"
    Push-Location $buildDir
    try {
        & make all -j8
        if ($LASTEXITCODE -ne 0) {
            throw "$Project build failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

Invoke-FirmwareBuild "Multicore_top_CPU0"
Invoke-FirmwareBuild "Multicore_top_CPU1"

$serverPython = Join-Path $repo "server\.venv\Scripts\python.exe"
if (-not (Test-Path $serverPython)) {
    $serverPython = "python"
}

& $serverPython -m unittest discover -s (Join-Path $repo "server\tests") -v
if ($LASTEXITCODE -ne 0) {
    throw "Server tests failed with exit code $LASTEXITCODE"
}

$pythonRoots = @(
    (Join-Path $repo "tools"),
    (Join-Path $repo "server\app"),
    (Join-Path $repo "server\tools"),
    (Join-Path $repo "server\tests")
) | Where-Object { Test-Path $_ }

$pythonFiles = @(
    Get-ChildItem -Path $pythonRoots -Recurse -File -Filter "*.py" |
        Where-Object { -not $_.Name.StartsWith("._") } |
        ForEach-Object { $_.FullName }
)

& python -m py_compile @pythonFiles
if ($LASTEXITCODE -ne 0) {
    throw "Python compile check failed with exit code $LASTEXITCODE"
}

if (Get-Command node -ErrorAction SilentlyContinue) {
    & node --check (Join-Path $repo "server\static\app.js")
    if ($LASTEXITCODE -ne 0) {
        throw "JavaScript syntax check failed with exit code $LASTEXITCODE"
    }
}

Push-Location $repo
try {
    & git diff --check
    if ($LASTEXITCODE -ne 0) {
        throw "git diff --check failed"
    }
}
finally {
    Pop-Location
}

Write-Host "All firmware and host checks passed."
