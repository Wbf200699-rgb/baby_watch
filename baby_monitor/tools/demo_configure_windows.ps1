$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
python (Join-Path $Root "tools\demo_fallback.py") configure --import-existing-build
