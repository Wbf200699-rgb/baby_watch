param(
    [string] $JLinkExe = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cpu0 = Join-Path $root "Multicore_top_CPU0\Debug\Multicore_top_CPU0_internal.srec"
$cpu1 = Join-Path $root "Multicore_top_CPU1\Debug\Multicore_top_CPU1.srec"
$commanderScript = Join-Path $PSScriptRoot "program_ra8_internal.jlink"
$imageBuilder = Join-Path $PSScriptRoot "build_vision_flash_images.ps1"

# The normal CPU0 build updates the ELF, but the dedicated internal-only S-record
# is a separate artifact. Always regenerate it so J-Link cannot flash a stale CPU0.
& $imageBuilder -Configuration "Debug"

if (-not (Test-Path $cpu0)) {
    throw "Internal CPU0 image was not generated: $cpu0"
}
if (-not (Test-Path $cpu1)) {
    throw "CPU1 image not found: $cpu1"
}

if (-not $JLinkExe) {
    $command = Get-Command JLink.exe -ErrorAction SilentlyContinue
    if ($command) {
        $JLinkExe = $command.Source
    } elseif (Test-Path "D:\Tools\SEGGER\JLink_V938a\JLink.exe") {
        $JLinkExe = "D:\Tools\SEGGER\JLink_V938a\JLink.exe"
    } else {
        throw "JLink.exe was not found"
    }
}

Push-Location $root
try {
    $output = & $JLinkExe -NoGui 1 -CommanderScript $commanderScript 2>&1
    $output | ForEach-Object { Write-Host $_ }
    $joinedOutput = $output -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "J-Link internal firmware programming failed with exit code $LASTEXITCODE"
    }
    $jlinkFailurePattern =
        "Cannot connect to the probe/programmer|" +
        "Connecting to J-Link via USB\.\.\.FAILED|" +
        "Error while programming flash|" +
        "Verify failed|" +
        "Script processing was aborted"
    if ($joinedOutput -match $jlinkFailurePattern) {
        throw "J-Link reported an internal firmware programming error"
    }
    $cpu0LoadOk = $joinedOutput -match
        "(?s)J-Link>loadfile Multicore_top_CPU0/Debug/Multicore_top_CPU0_internal\.srec.*?O\.K\."
    $cpu1LoadOk = $joinedOutput -match
        "(?s)J-Link>loadfile Multicore_top_CPU1/Debug/Multicore_top_CPU1\.srec.*?O\.K\."
    if (-not ($cpu0LoadOk -and $cpu1LoadOk)) {
        throw "J-Link did not confirm both CPU image load operations"
    }
    if ($joinedOutput -notmatch "Script processing completed\.") {
        throw "J-Link did not complete the internal firmware programming script"
    }
} finally {
    Pop-Location
}
