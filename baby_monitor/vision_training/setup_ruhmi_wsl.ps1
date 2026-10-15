param(
    [string]$Distro = "Ubuntu",
    [switch]$SkipApt
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootWsl = (& wsl.exe -d $Distro -- wslpath -a $Root.Replace('\', '/')).Trim()
if ($LASTEXITCODE -ne 0 -or -not $RootWsl) {
    throw "Could not map the project path into WSL: $Root"
}

if (-not $SkipApt) {
    & wsl.exe -d $Distro -u root -- bash -lc `
        "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y python3 python3-venv python3-pip ca-certificates"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install the Python bootstrap environment in WSL."
    }
}

& wsl.exe -d $Distro -- bash "$RootWsl/setup_ruhmi_wsl.sh"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create the WSL RUHMI/MERA compiler environment."
}
Write-Host "WSL RUHMI/MERA compiler environment is ready."
