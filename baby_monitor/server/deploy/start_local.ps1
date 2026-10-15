param(
    [int]$Port = 8088,
    [string]$DeviceKeys = "demo-device:dev-secret"
)

$ErrorActionPreference = "Stop"
$env:LOCAL_PORT = "$Port"
$env:BABY_MONITOR_DEVICE_KEYS = $DeviceKeys
$env:ADMIN_USER = "unused"
$env:ADMIN_PASSWORD_HASH = "unused"
docker compose -f compose.yaml -f compose.local.yaml up -d --build app
$ip = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
    $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*"
} | Select-Object -First 1 -ExpandProperty IPAddress)
Write-Host "Local dashboard: http://127.0.0.1:$Port"
if ($ip) { Write-Host "Hotspot/LAN ESP32 endpoint: http://${ip}:$Port" }
