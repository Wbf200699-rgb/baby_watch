param(
    [string]$Port = "",
    [switch]$CheckOnly
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Arguments = @((Join-Path $Root "tools\demo_fallback.py"), "flash", "cloud")
if ($Port) { $Arguments += @("--port", $Port) }
if ($CheckOnly) { $Arguments += "--check-only" }
python @Arguments
