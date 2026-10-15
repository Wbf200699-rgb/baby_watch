param(
    [string]$HostName = "100.81.192.84",
    [string]$UserName = "zyx",
    [string]$RemoteDir = "/home/zyx/baby_monitor/server",
    [string]$PublicUrl = "https://renesas.zyxserver.online",
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"
$ServerDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Archive = Join-Path $env:TEMP "baby-monitor-server.tgz"
$Checksum = "$Archive.sha256"

foreach ($required in @("Dockerfile", "compose.yaml", "compose.pi.yaml", "requirements.txt", "app\main.py")) {
    if (-not (Test-Path (Join-Path $ServerDir $required))) {
        throw "Server deployment input is missing: $required"
    }
}
foreach ($command in @("tar", "scp", "ssh")) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "Required command is not available: $command"
    }
}
if ($CheckOnly) {
    Write-Host "Deployment preflight OK. No archive was uploaded and no remote service was changed."
    exit 0
}

Remove-Item $Archive, $Checksum -ErrorAction SilentlyContinue

try {
    tar -czf $Archive --exclude=.venv --exclude=data --exclude=__pycache__ `
        --exclude=.pytest_cache --exclude='*.pyc' -C $ServerDir .
    if ($LASTEXITCODE -ne 0) { throw "Failed to create deployment archive" }

    $Hash = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
    "$Hash  baby-monitor-server.tgz" | Set-Content -Encoding ascii -NoNewline $Checksum

    scp $Archive $Checksum "${UserName}@${HostName}:/tmp/"
    if ($LASTEXITCODE -ne 0) { throw "Failed to upload deployment archive" }

    $Command = @"
set -e
cd /tmp
sha256sum -c baby-monitor-server.tgz.sha256
mkdir -p '$RemoteDir'
cd '$RemoteDir'
test -f .env && cp .env /tmp/baby-monitor-server.env || true
tar xzf /tmp/baby-monitor-server.tgz
test -f /tmp/baby-monitor-server.env && mv /tmp/baby-monitor-server.env .env
test -f .env
docker compose -f compose.yaml -f compose.pi.yaml up -d --build --remove-orphans --wait --wait-timeout 120 app
docker compose -f compose.yaml -f compose.pi.yaml ps
curl -fsS --max-time 10 http://127.0.0.1:8088/health >/dev/null
"@
    # PowerShell here-strings use CRLF on Windows. A literal carriage return
    # passed as an SSH command becomes part of Bash tokens and paths.
    $Command = $Command.Replace("`r", "")
    ssh "${UserName}@${HostName}" $Command
    if ($LASTEXITCODE -ne 0) { throw "Remote deployment or Pi health check failed" }

    $Healthy = $false
    for ($attempt = 1; $attempt -le 12; $attempt++) {
        try {
            $response = Invoke-RestMethod -Uri "$($PublicUrl.TrimEnd('/'))/health" -TimeoutSec 10
            if ($response.ok -eq $true) {
                $Healthy = $true
                break
            }
        } catch {
            if ($attempt -eq 12) { throw }
        }
        Start-Sleep -Seconds 5
    }
    if (-not $Healthy) { throw "Public server health check did not become ready" }
    Write-Host "Deployment verified on the Pi and at $PublicUrl/health"
} finally {
    Remove-Item $Archive, $Checksum -ErrorAction SilentlyContinue
}
