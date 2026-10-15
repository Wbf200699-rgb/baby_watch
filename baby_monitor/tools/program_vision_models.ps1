param(
    [string] $JLinkExe = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$modelBin = Join-Path $root "Multicore_top_CPU0\Debug\vision_models_ospi0_cs0.bin"
$commanderScript = Join-Path $PSScriptRoot "program_vision_models.jlink"

if (-not (Test-Path $modelBin)) {
    throw "Model image not found. Run tools\build_vision_flash_images.ps1 first."
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
        throw "J-Link model programming failed with exit code $LASTEXITCODE"
    }
    $jlinkFailurePattern =
        "Cannot connect to the probe/programmer|" +
        "Connecting to J-Link via USB\.\.\.FAILED|" +
        "Script processing was aborted"
    if ($joinedOutput -match $jlinkFailurePattern) {
        throw "J-Link reported a model programming error"
    }
    $verifyCommand = $joinedOutput.LastIndexOf("J-Link>verifybin")
    $verifySuccess = $joinedOutput.LastIndexOf("Verify successful.")
    $scriptCompleted = $joinedOutput.LastIndexOf("Script processing completed.")
    if (($verifyCommand -lt 0) -or
        ($verifySuccess -lt $verifyCommand) -or
        ($scriptCompleted -lt $verifySuccess)) {
        throw "J-Link did not confirm the explicit OSPI model verification"
    }
} finally {
    Pop-Location
}

