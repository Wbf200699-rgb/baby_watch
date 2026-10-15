param(
    [Parameter(Mandatory = $true)][string]$Model,
    [ValidateSet("npu", "cpu")][string]$Target = "npu",
    [string]$Output = "",
    [string]$Suffix = "",
    [string]$Distro = "Ubuntu",
    [switch]$External
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ModelFull = (Resolve-Path $Model).Path
if (-not $Output) {
    $ModelStem = [System.IO.Path]::GetFileNameWithoutExtension($ModelFull)
    $Output = Join-Path $Root "artifacts\ruhmi\$($ModelStem)_$Target"
}
if (-not [System.IO.Path]::IsPathRooted($Output)) {
    $Output = Join-Path (Get-Location).Path $Output
}
$OutputFull = [System.IO.Path]::GetFullPath($Output)

$RootWsl = (& wsl.exe -d $Distro -- wslpath -a $Root.Replace('\', '/')).Trim()
$ModelWsl = (& wsl.exe -d $Distro -- wslpath -a $ModelFull.Replace('\', '/')).Trim()
$OutputWsl = (& wsl.exe -d $Distro -- wslpath -a $OutputFull.Replace('\', '/')).Trim()
if ($LASTEXITCODE -ne 0 -or -not $RootWsl -or -not $ModelWsl -or -not $OutputWsl) {
    throw "Could not map one or more paths into WSL."
}

$Arguments = @(
    "$RootWsl/compile_ruhmi_wsl.sh",
    "--model", $ModelWsl,
    "--output", $OutputWsl,
    "--target", $Target
)
if ($Suffix) {
    $Arguments += @("--suffix", $Suffix)
}
if ($External) {
    $Arguments += "--external"
}
& wsl.exe -d $Distro -- bash @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "RUHMI $Target compilation failed."
}
Write-Host "RUHMI $Target output: $OutputFull"
