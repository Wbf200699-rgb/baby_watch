$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$SyntaxErrors = @()
Get-ChildItem -Path $Root -Filter "*.ps1" | ForEach-Object {
    $Tokens = $null
    $Errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $_.FullName,
        [ref]$Tokens,
        [ref]$Errors
    )
    foreach ($ErrorItem in $Errors) {
        $SyntaxErrors += "$($_.Name):$($ErrorItem.Extent.StartLineNumber): $($ErrorItem.Message)"
    }
}
if ($SyntaxErrors.Count -gt 0) {
    $SyntaxErrors | ForEach-Object { Write-Error $_ }
    throw "PowerShell syntax validation failed."
}

$Python = Join-Path $Root ".venv\Scripts\python.exe"
if (-not (Test-Path $Python)) {
    throw "Run .\setup.ps1 first."
}
& $Python -m pytest tests -q
if ($LASTEXITCODE -ne 0) {
    throw "Python tests failed."
}
& $Python scene_dataset_preflight.py --stage base
if ($LASTEXITCODE -ne 0) {
    throw "Public scene dataset base preflight failed."
}
Write-Host "PowerShell syntax, Python tests, and public dataset base preflight passed."
