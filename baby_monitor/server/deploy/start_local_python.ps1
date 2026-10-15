param(
    [int]$Port = 8088,
    [string]$DeviceKeys = "demo-device:dev-secret",
    [string]$AdminUser = "",
    [string]$AdminPassword = ""
)

$ErrorActionPreference = "Stop"
$ServerDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Python = Join-Path $ServerDir ".venv\Scripts\python.exe"

if (-not (Test-Path $Python)) {
    python -m venv (Join-Path $ServerDir ".venv")
    & $Python -m pip install -r (Join-Path $ServerDir "requirements.txt")
}

$env:BABY_MONITOR_DEVICE_KEYS = $DeviceKeys
$env:BABY_MONITOR_ADMIN_USER = $AdminUser
$env:BABY_MONITOR_ADMIN_PASSWORD = $AdminPassword
Set-Location $ServerDir
Write-Host "Dashboard: http://127.0.0.1:$Port"
Write-Host "ESP32 on the same hotspot should use this PC's hotspot IPv4 address and port $Port."
& $Python -m uvicorn app.main:app --host 0.0.0.0 --port $Port
