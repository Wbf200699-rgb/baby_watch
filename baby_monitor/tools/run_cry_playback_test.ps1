param(
    [Parameter(Mandatory = $true)][string]$Audio,
    [Parameter(Mandatory = $true)][string]$Output,
    [ValidateRange(0, 100)][int]$Volume = 20,
    [ValidateSet("neutral", "expect-cry", "expect-no-cry")]
    [string]$Expectation = "neutral",
    [ValidateRange(0, 2400)][int]$Samples = 0,
    [ValidateRange(50, 2000)][int]$IntervalMs = 250,
    [ValidateRange(1, 30)][int]$LeadSeconds = 4,
    [ValidateRange(0, 1000)][int]$AllowedRawHits = 0,
    [ValidateRange(0, 1000)][int]$AllowedConfirmedSamples = 0,
    [ValidateRange(0, 1000)][int]$RawThreshold = 0,
    [ValidateRange(1, 30)][int]$PreflightSeconds = 3,
    [ValidateRange(0, 5000)][int]$StartupAllowanceMs = 1000,
    [ValidateRange(1.0, 20.0)][double]$MinimumMicMeanRatio = 1.25,
    [ValidateRange(0, 1000000)][int]$MinimumMicMeanDelta = 128,
    [ValidateRange(1, 1000)][int]$MinimumExposedSamples = 2,
    [switch]$NoLoudnessNormalize
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$audioPath = (Resolve-Path -LiteralPath $Audio).Path
$outputPath = if ([System.IO.Path]::IsPathRooted($Output)) {
    [System.IO.Path]::GetFullPath($Output)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $Output))
}
$monitor = Join-Path $PSScriptRoot "monitor_cry_jlink.py"
$ffplay = "D:\Tools\ffmpeg\bin\ffplay.exe"
$ffprobe = "D:\Tools\ffmpeg\bin\ffprobe.exe"
$python = (Get-Command python.exe -ErrorAction Stop).Source
$firmwareSource = Join-Path $root "Multicore_top_CPU0\src\thread_code\audio_cry_inference.c"

if (-not (Test-Path -LiteralPath $monitor)) {
    throw "J-Link monitor was not found: $monitor"
}
if (-not (Test-Path -LiteralPath $ffplay)) {
    throw "ffplay was not found: $ffplay"
}
if (-not (Test-Path -LiteralPath $ffprobe)) {
    throw "ffprobe was not found: $ffprobe"
}
if ($RawThreshold -eq 0) {
    $sourceText = Get-Content -LiteralPath $firmwareSource -Raw
    $thresholdMatch = [regex]::Match(
        $sourceText,
        "#define\s+CRY_DECISION_THRESHOLD_X1000\s+\((\d+)U\)"
    )
    if (-not $thresholdMatch.Success) {
        throw "Could not read CRY_DECISION_THRESHOLD_X1000 from $firmwareSource"
    }
    $RawThreshold = [int]$thresholdMatch.Groups[1].Value
}
$durationText = & $ffprobe `
    -v error `
    -show_entries format=duration `
    -of default=noprint_wrappers=1:nokey=1 `
    $audioPath
if ($LASTEXITCODE -ne 0) {
    throw "ffprobe failed with exit code $LASTEXITCODE"
}
$durationSeconds = [double]::Parse(
    $durationText.Trim(),
    [System.Globalization.CultureInfo]::InvariantCulture
)
$requiredSamples = [int][Math]::Ceiling(
    (($LeadSeconds + $durationSeconds + 2.0) * 1000.0) / $IntervalMs
)
if ($Samples -eq 0) {
    $Samples = $requiredSamples
} elseif ($Samples -lt $requiredSamples) {
    throw (
        "Samples=$Samples ends monitoring before playback completes; " +
        "need at least $requiredSamples for ${durationSeconds}s audio."
    )
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$runStartedAt = Get-Date
$preflightPath = Join-Path $outputPath "preflight"
$monitorPath = Join-Path $outputPath "monitor"
$preflightSamples = [int][Math]::Ceiling(
    ($PreflightSeconds * 1000.0) / $IntervalMs
)
$preflightArguments = @(
    $monitor,
    "--samples", $preflightSamples,
    "--interval-ms", $IntervalMs,
    "--output", $preflightPath,
    "--raw-threshold", $RawThreshold,
    "--expect-no-cry",
    "--allowed-raw-hits", 0,
    "--allowed-confirmed-samples", 0
)
& $python @preflightArguments
if ($LASTEXITCODE -ne 0) {
    throw "Quiet preflight failed; playback was not started."
}
$preflightSummary = Get-ChildItem -LiteralPath $preflightPath -Recurse -Filter "summary.json" |
    Where-Object { $_.LastWriteTime -ge $runStartedAt.AddSeconds(-1) } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $preflightSummary) {
    throw "Quiet preflight produced no current summary below $preflightPath"
}
$preflightData = Get-Content -LiteralPath $preflightSummary.FullName -Raw |
    ConvertFrom-Json
$stdout = Join-Path $outputPath "monitor_stdout.log"
$stderr = Join-Path $outputPath "monitor_stderr.log"
$arguments = @(
    $monitor,
    "--samples", $Samples,
    "--interval-ms", $IntervalMs,
    "--output", $monitorPath,
    "--raw-threshold", $RawThreshold
)
if ($Expectation -eq "expect-no-cry") {
    $arguments += @(
        "--expect-no-cry",
        "--allowed-raw-hits", $AllowedRawHits,
        "--allowed-confirmed-samples", $AllowedConfirmedSamples
    )
}

$monitorFailure = $null
$monitorProcess = Start-Process `
    -FilePath $python `
    -ArgumentList $arguments `
    -WorkingDirectory $root `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr `
    -PassThru
try {
    Start-Sleep -Seconds $LeadSeconds
    $playbackStartedAt = Get-Date
    $playArguments = @(
        "-hide_banner",
        "-loglevel", "error",
        "-nodisp",
        "-autoexit",
        "-volume", $Volume
    )
    if (-not $NoLoudnessNormalize) {
        $playArguments += @(
            "-af",
            "loudnorm=I=-16:LRA=11:TP=-1.5"
        )
    }
    $playArguments += $audioPath
    & $ffplay @playArguments
    if ($LASTEXITCODE -ne 0) {
        throw "ffplay failed with exit code $LASTEXITCODE"
    }
    $timeoutSeconds = [int][Math]::Ceiling(
        ($Samples * $IntervalMs) / 1000.0 + 30.0
    )
    if (-not $monitorProcess.HasExited) {
        Wait-Process -Id $monitorProcess.Id -Timeout $timeoutSeconds
    }
    [void]$monitorProcess.WaitForExit()
    $monitorProcess.Refresh()
    $monitorExitCode = $monitorProcess.ExitCode
    if (($null -ne $monitorExitCode) -and ($monitorExitCode -ne 0)) {
        $detail = if (Test-Path -LiteralPath $stderr) {
            Get-Content -LiteralPath $stderr -Raw
        } else {
            ""
        }
        $monitorFailure = "J-Link monitor exited with ${monitorExitCode}: $detail"
    }
} finally {
    if (-not $monitorProcess.HasExited) {
        Stop-Process -Id $monitorProcess.Id -Force
    }
}

$summary = Get-ChildItem -LiteralPath $monitorPath -Recurse -Filter "summary.json" |
    Where-Object { $_.LastWriteTime -ge $runStartedAt.AddSeconds(-1) } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $summary) {
    throw "J-Link monitor produced no summary below $outputPath"
}
$summaryData = Get-Content -LiteralPath $summary.FullName -Raw | ConvertFrom-Json
$sampleFile = Join-Path $summary.Directory.FullName "samples.csv"
$sampleRows = @(Import-Csv -LiteralPath $sampleFile)
$guardMilliseconds = [Math]::Max(
    $IntervalMs,
    $LeadSeconds * 1000 - $StartupAllowanceMs
)
$guardSamples = [Math]::Min(
    $sampleRows.Count,
    [Math]::Max(1, [int][Math]::Floor($guardMilliseconds / $IntervalMs))
)
$leadRows = @($sampleRows | Select-Object -First $guardSamples)
$startedUnconfirmed = (
    $leadRows.Count -eq $guardSamples -and
    @($leadRows | Where-Object { [int]$_.g_cry_confirmed -ne 0 }).Count -eq 0
)
$baselineMean = [double]$preflightData.mic_mean_abs_p95
$exposureMeanThreshold = [int][Math]::Ceiling(
    [Math]::Max(
        $baselineMean + $MinimumMicMeanDelta,
        $baselineMean * $MinimumMicMeanRatio
    )
)
$exposedIndices = @()
for ($index = $guardSamples; $index -lt $sampleRows.Count; $index++) {
    if ([int]$sampleRows[$index].mic_mean_abs -ge $exposureMeanThreshold) {
        $exposedIndices += $index
    }
}
$exposureVerified = $exposedIndices.Count -ge $MinimumExposedSamples
$confirmationTransitionIndices = @()
for ($index = 1; $index -lt $sampleRows.Count; $index++) {
    if (
        [int]$sampleRows[$index - 1].g_cry_confirmed -eq 0 -and
        [int]$sampleRows[$index].g_cry_confirmed -ne 0
    ) {
        $confirmationTransitionIndices += $index
    }
}
$firstExposureIndex = if ($exposedIndices.Count) {
    [int]$exposedIndices[0]
} else {
    $null
}
$newConfirmationObserved = $false
if ($startedUnconfirmed -and $exposureVerified) {
    $newConfirmationObserved = @(
        $confirmationTransitionIndices | Where-Object {
            $_ -ge ([int]$firstExposureIndex - 1)
        }
    ).Count -gt 0
}
$playbackMetadata = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    audio = $audioPath
    audio_sha256 = (Get-FileHash -LiteralPath $audioPath -Algorithm SHA256).Hash.ToLowerInvariant()
    volume_percent = $Volume
    loudness_normalized = (-not $NoLoudnessNormalize)
    expectation = $Expectation
    audio_duration_seconds = $durationSeconds
    lead_seconds = $LeadSeconds
    monitor_samples = $Samples
    monitor_interval_ms = $IntervalMs
    raw_threshold = $RawThreshold
    preflight_summary = $preflightSummary.FullName
    preflight_samples = $preflightSamples
    preflight_mic_mean_abs_p95 = $baselineMean
    startup_allowance_ms = $StartupAllowanceMs
    guarded_lead_samples = $guardSamples
    playback_started_at = $playbackStartedAt.ToString("o")
    monitor_summary = $summary.FullName
    monitor_exit_code = $monitorExitCode
    monitor_failure = $monitorFailure
    started_unconfirmed = $startedUnconfirmed
    mic_mean_abs_exposure_threshold = $exposureMeanThreshold
    exposed_samples = $exposedIndices.Count
    minimum_exposed_samples = $MinimumExposedSamples
    first_exposure_sample_index = $firstExposureIndex
    exposure_verified = $exposureVerified
    confirmation_transition_indices = $confirmationTransitionIndices
    new_confirmation_observed = $newConfirmationObserved
}
$playbackMetadata |
    ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $outputPath "playback.json") -Encoding UTF8
if ($monitorFailure) {
    throw $monitorFailure
}
if (-not $startedUnconfirmed) {
    throw "The detector was already confirmed during the guarded quiet lead."
}
if (-not $exposureVerified) {
    throw (
        "Playback was not proven at the microphone: only " +
        "$($exposedIndices.Count) sample(s) exceeded mean_abs " +
        "$exposureMeanThreshold."
    )
}
if (($Expectation -eq "expect-cry") -and (-not $newConfirmationObserved)) {
    throw (
        "Expected cry playback did not produce a new confirmed transition " +
        "after microphone exposure began."
    )
}
if (
    ($Expectation -eq "expect-no-cry") -and
    (-not [bool]$summaryData.pass)
) {
    throw "Non-cry playback exceeded the configured false-positive limits."
}
Get-Content -LiteralPath $summary.FullName -Raw
