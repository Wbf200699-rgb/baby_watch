param(
    [int] $TargetVisionFrames = 50,
    [int] $PollIntervalMs = 500,
    [int] $TimeoutMinutes = 10,
    [string] $CredentialsFile = "",
    [string] $OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $CredentialsFile) {
    $CredentialsFile = Join-Path $repo "server\data\deployment_credentials.txt"
}
if (-not $OutputDirectory) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputDirectory = Join-Path $repo "test_results\hardware_$stamp"
}

$settings = @{}
Get-Content -LiteralPath $CredentialsFile | ForEach-Object {
    if ($_ -match '^([^#=]+)=(.*)$') {
        $settings[$matches[1].Trim()] = $matches[2].Trim()
    }
}
foreach ($required in @("URL", "ADMIN_USER", "ADMIN_PASSWORD", "DEVICE_ID")) {
    if (-not $settings[$required]) {
        throw "Missing $required in $CredentialsFile"
    }
}

$credentialText = "$($settings.ADMIN_USER):$($settings.ADMIN_PASSWORD)"
$authorization = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($credentialText))
$headers = @{ Authorization = "Basic $authorization" }
$snapshotUrl = "$($settings.URL)/api/dashboard/snapshot?device_id=$($settings.DEVICE_ID)"
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
$samples = [Collections.Generic.List[object]]::new()
$lastFrame = -1L
$lastSnapshot = $null

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

while (($samples.Count -lt $TargetVisionFrames) -and ((Get-Date) -lt $deadline)) {
    try {
        $snapshot = Invoke-RestMethod -Uri $snapshotUrl -Headers $headers -TimeoutSec 10
        $lastSnapshot = $snapshot
        $telemetry = $snapshot.telemetry
        $vision = $telemetry.vision
        $camera = $telemetry.sensors.camera
        $device = $snapshot.devices | Where-Object device_id -eq $settings.DEVICE_ID | Select-Object -First 1

        if (($null -ne $vision) -and ([int64] $vision.f -gt 0) -and ([int64] $vision.f -ne $lastFrame)) {
            $timing = @($vision.t)
            $errors = @($vision.e)
            $samples.Add([pscustomobject]@{
                observed_at             = $snapshot.server_time
                device_online           = [bool] $device.online
                ra8_status_age_ms        = [int64] $telemetry.health.ra8_status_age_ms
                ra8_firmware             = [int64] $telemetry.health.ra8_link.fw
                vision_firmware          = [int64] $telemetry.health.ra8_link.vision_fw
                camera_recording         = [int64] $camera.rec
                camera_auto              = [int64] $camera.auto
                camera_auto_ms           = [int64] $camera.auto_ms
                camera_frames            = [int64] $camera.frames
                camera_saved             = [int64] $camera.saved
                camera_errors            = [int64] $camera.frame_err
                camera_timeouts          = [int64] $camera.to
                camera_uart_errors       = [int64] $camera.uart_err
                camera_state             = [int64] $camera.s
                vision_frame             = [int64] $vision.f
                vision_inference_count   = [int64] $vision.n
                vision_state             = [int64] $vision.s
                vision_ready_mask        = [int64] $vision.r
                jpeg_errors              = [int64] $errors[0]
                npu_errors               = [int64] $errors[1]
                dropped_frames           = [int64] $errors[2]
                decode_ms                = [int64] $timing[0]
                preprocess_ms            = [int64] $timing[1]
                scene_ms                 = [int64] $timing[2]
                occlusion_ms             = [int64] $timing[3]
                behavior_ms              = [int64] $timing[4]
                total_ms                 = [int64] $timing[5]
                event_queue_drops        = [int64] $telemetry.health.event_queue_drops
                gateway_http_failures    = [int64] $telemetry.health.gateway.http_failures
                gateway_uart_errors      = [int64] $telemetry.health.ra8_link.err
                free_internal_heap       = [int64] $telemetry.health.free_internal_heap
                free_psram               = [int64] $telemetry.health.free_psram
            })
            $lastFrame = [int64] $vision.f
            Write-Host ("Captured vision frame {0} ({1}/{2})" -f $lastFrame, $samples.Count, $TargetVisionFrames)
        }
    }
    catch {
        Write-Warning $_.Exception.Message
    }

    if ($samples.Count -lt $TargetVisionFrames) {
        Start-Sleep -Milliseconds $PollIntervalMs
    }
}

if ($samples.Count -eq 0) {
    throw "No vision frames were observed before timeout"
}

function Get-Delta([string] $Property) {
    return [int64] $samples[$samples.Count - 1].$Property - [int64] $samples[0].$Property
}

function Get-Average([string] $Property) {
    return [Math]::Round(($samples | Measure-Object -Property $Property -Average).Average, 2)
}

function Get-P95([string] $Property) {
    $values = @($samples | ForEach-Object { [double] $_.$Property } | Sort-Object)
    $index = [Math]::Max(0, [Math]::Ceiling($values.Count * 0.95) - 1)
    return $values[$index]
}

$first = $samples[0]
$last = $samples[$samples.Count - 1]
$firstObserved = [DateTimeOffset]::Parse([string] $first.observed_at)
$lastObserved = [DateTimeOffset]::Parse([string] $last.observed_at)
$elapsedSeconds = [Math]::Max(0.0, ($lastObserved - $firstObserved).TotalSeconds)
$frameIntervals = [Math]::Max(0L, ([int64] $last.vision_frame - [int64] $first.vision_frame))
$effectiveFps = if (($elapsedSeconds -gt 0.0) -and ($frameIntervals -gt 0L)) {
    [Math]::Round($frameIntervals / $elapsedSeconds, 3)
} else {
    0.0
}
$averageFrameIntervalMs = if ($frameIntervals -gt 0L) {
    [Math]::Round(($elapsedSeconds * 1000.0) / $frameIntervals, 2)
} else {
    0.0
}
$summary = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    device_id = $settings.DEVICE_ID
    target_frames = $TargetVisionFrames
    captured_frames = $samples.Count
    first_frame = $first.vision_frame
    last_frame = $last.vision_frame
    elapsed_seconds = [Math]::Round($elapsedSeconds, 3)
    effective_fps = $effectiveFps
    average_frame_interval_ms = $averageFrameIntervalMs
    inference_delta = Get-Delta "vision_inference_count"
    camera_frame_delta = Get-Delta "camera_frames"
    camera_saved_delta = Get-Delta "camera_saved"
    camera_error_delta = Get-Delta "camera_errors"
    camera_timeout_delta = Get-Delta "camera_timeouts"
    camera_uart_error_delta = Get-Delta "camera_uart_errors"
    jpeg_error_delta = Get-Delta "jpeg_errors"
    npu_error_delta = Get-Delta "npu_errors"
    dropped_frame_delta = Get-Delta "dropped_frames"
    max_status_age_ms = ($samples | Measure-Object -Property ra8_status_age_ms -Maximum).Maximum
    max_event_queue_drops = ($samples | Measure-Object -Property event_queue_drops -Maximum).Maximum
    max_gateway_http_failures = ($samples | Measure-Object -Property gateway_http_failures -Maximum).Maximum
    max_gateway_uart_errors = ($samples | Measure-Object -Property gateway_uart_errors -Maximum).Maximum
    minimum_free_internal_heap = ($samples | Measure-Object -Property free_internal_heap -Minimum).Minimum
    minimum_free_psram = ($samples | Measure-Object -Property free_psram -Minimum).Minimum
    timings_ms = [ordered]@{
        decode_average = Get-Average "decode_ms"
        decode_p95 = Get-P95 "decode_ms"
        preprocess_average = Get-Average "preprocess_ms"
        preprocess_p95 = Get-P95 "preprocess_ms"
        scene_average = Get-Average "scene_ms"
        scene_p95 = Get-P95 "scene_ms"
        occlusion_average = Get-Average "occlusion_ms"
        occlusion_p95 = Get-P95 "occlusion_ms"
        behavior_average = Get-Average "behavior_ms"
        behavior_p95 = Get-P95 "behavior_ms"
        total_average = Get-Average "total_ms"
        total_p95 = Get-P95 "total_ms"
    }
    pass = ($samples.Count -ge $TargetVisionFrames) -and
           ($samples | Where-Object { -not $_.device_online }).Count -eq 0 -and
           ($samples | Where-Object { $_.camera_auto -ne 1 -or $_.vision_ready_mask -ne 7 }).Count -eq 0 -and
           ((Get-Delta "camera_saved") -eq 0) -and
           ((Get-Delta "camera_errors") -eq 0) -and
           ((Get-Delta "camera_timeouts") -eq 0) -and
           ((Get-Delta "jpeg_errors") -eq 0) -and
           ((Get-Delta "npu_errors") -eq 0)
}

$samples | Export-Csv -LiteralPath (Join-Path $OutputDirectory "vision_frames.csv") -NoTypeInformation -Encoding UTF8
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory "summary.json") -Encoding UTF8
if ($null -ne $lastSnapshot) {
    $lastSnapshot | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $OutputDirectory "last_snapshot.json") -Encoding UTF8
}

$summary | ConvertTo-Json -Depth 6
if (-not $summary.pass) {
    exit 2
}
