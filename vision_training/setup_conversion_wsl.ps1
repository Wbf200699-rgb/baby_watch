param(
    [string]$Distro = "Ubuntu",
    [switch]$SkipApt
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootPortable = $Root.Replace('\', '/')
$RootWsl = (& wsl.exe -d $Distro -- wslpath -a $RootPortable).Trim()
if ($LASTEXITCODE -ne 0 -or -not $RootWsl) {
    throw "Could not map the project path into WSL: $Root"
}

if (-not $SkipApt) {
    & wsl.exe -d $Distro -u root -- bash -lc `
        "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y python3-venv python3-pip cmake"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install Python venv support in WSL."
    }
}

& wsl.exe -d $Distro -- bash "$RootWsl/setup_conversion_wsl.sh"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create the WSL conversion environment."
}
Write-Host "WSL onnxsim/onnx2tf environment is ready."
