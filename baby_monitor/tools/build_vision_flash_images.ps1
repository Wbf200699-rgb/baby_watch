param(
    [string] $Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "Multicore_top_CPU0\$Configuration"
$elf = Join-Path $buildDir "Multicore_top_CPU0.elf"
$internalSrec = Join-Path $buildDir "Multicore_top_CPU0_internal.srec"
$modelBin = Join-Path $buildDir "vision_models_ospi0_cs0.bin"

if (-not (Test-Path $elf)) {
    throw "CPU0 ELF not found: $elf"
}

$objcopy = Get-Command arm-none-eabi-objcopy.exe -ErrorAction SilentlyContinue
if ($objcopy) {
    $objcopyPath = $objcopy.Source
} else {
    $fallback = "C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-objcopy.exe"
    if (-not (Test-Path $fallback)) {
        throw "arm-none-eabi-objcopy.exe was not found"
    }
    $objcopyPath = $fallback
}
$objdumpPath = Join-Path (Split-Path -Parent $objcopyPath) "arm-none-eabi-objdump.exe"
if (-not (Test-Path $objdumpPath)) {
    throw "arm-none-eabi-objdump.exe was not found next to objcopy"
}

$sectionLine = & $objdumpPath -h $elf | Select-String -Pattern '^\s*\d+\s+\.vision_models\s+([0-9A-Fa-f]+)\s+'
if (-not $sectionLine -or $sectionLine.Matches.Count -ne 1) {
    throw "Could not read .vision_models size from CPU0 ELF"
}
$expectedModelBytes = [Convert]::ToInt64($sectionLine.Matches[0].Groups[1].Value, 16)
if ($expectedModelBytes -le 0) {
    throw "CPU0 ELF contains an empty .vision_models section"
}

& $objcopyPath -O srec --remove-section=.vision_models $elf $internalSrec
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create the internal-only CPU0 S-record"
}

& $objcopyPath -O binary --only-section=.vision_models $elf $modelBin
if ($LASTEXITCODE -ne 0) {
    throw "Failed to extract the OSPI model image"
}

$actualModelBytes = (Get-Item $modelBin).Length
if ($actualModelBytes -ne $expectedModelBytes) {
    throw "Model image size $actualModelBytes does not match ELF section size $expectedModelBytes"
}

$externalRecords = Select-String -Path $internalSrec -Pattern '^S3..8[0-9A-Fa-f]{7}'
if ($externalRecords) {
    throw "Internal CPU0 S-record still contains OSPI addresses"
}

Write-Host "Internal CPU0 image: $internalSrec"
Write-Host "OSPI0/CS0 model image: $modelBin ($actualModelBytes bytes)"
