param(
    [Parameter(Mandatory = $true)]
    [string]$WifiSsid,
    [string]$Server = "https://renesas.zyxserver.online",
    [string]$Port = "",
    [switch]$BuildOnly,
    [switch]$CheckOnly,
    [switch]$SaveWifi
)

$ErrorActionPreference = "Stop"
$Arguments = @(
    (Join-Path $PSScriptRoot "flash_p4_wifi.py"),
    "--ssid", $WifiSsid,
    "--server", $Server
)
if ($Port) { $Arguments += @("--port", $Port) }
if ($BuildOnly) { $Arguments += "--build-only" }
if ($CheckOnly) { $Arguments += "--check-only" }
if ($SaveWifi) { $Arguments += "--save-wifi" }

python @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "P4 Wi-Fi build/flash failed with exit code $LASTEXITCODE"
}
