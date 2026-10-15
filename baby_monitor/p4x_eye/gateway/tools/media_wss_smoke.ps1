param(
    [ValidateSet("video", "audio")]
    [string]$Kind = "video",
    [string]$DeviceId = "esp32cam-01",
    [string]$BaseUrl = "wss://renesas.zyxserver.online",
    [Parameter(Mandatory = $true)]
    [string]$User,
    [Parameter(Mandatory = $true)]
    [string]$Password,
    [int]$DurationSeconds = 15,
    [string]$SaveLatestJpeg = ""
)

$ErrorActionPreference = "Stop"

function Read-Be16([byte[]]$Data, [int]$Offset) {
    return ([uint16]$Data[$Offset] -shl 8) -bor [uint16]$Data[$Offset + 1]
}

function Read-Be32([byte[]]$Data, [int]$Offset) {
    return ([uint32]$Data[$Offset] -shl 24) -bor
           ([uint32]$Data[$Offset + 1] -shl 16) -bor
           ([uint32]$Data[$Offset + 2] -shl 8) -bor
           [uint32]$Data[$Offset + 3]
}

$path = "/api/dashboard/media/$([uri]::EscapeDataString($DeviceId))/$Kind/ws"
$uri = [Uri]($BaseUrl.TrimEnd("/") + $path)
$socket = [System.Net.WebSockets.ClientWebSocket]::new()
$basic = [Convert]::ToBase64String(
    [Text.Encoding]::UTF8.GetBytes("${User}:${Password}")
)
$socket.Options.SetRequestHeader("Authorization", "Basic $basic")
$socket.Options.KeepAliveInterval = [TimeSpan]::FromSeconds(10)

$connectCts = [Threading.CancellationTokenSource]::new(
    [TimeSpan]::FromSeconds(15)
)
$socket.ConnectAsync($uri, $connectCts.Token).GetAwaiter().GetResult() |
    Out-Null

$deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
$receiveBuffer = [byte[]]::new(65536)
$frames = 0
$invalid = 0
$gaps = 0
$bytes = [uint64]0
$firstSequence = $null
$lastSequence = $null
$firstFrameAt = $null
$lastFrameAt = $null
$width = 0
$height = 0
$sampleRate = 0
$latestJpeg = $null

try {
    while ([DateTime]::UtcNow -lt $deadline) {
        $message = [IO.MemoryStream]::new()
        do {
            $remaining = $deadline - [DateTime]::UtcNow
            if ($remaining -le [TimeSpan]::Zero) {
                break
            }
            $receiveCts = [Threading.CancellationTokenSource]::new($remaining)
            try {
                $segment = [ArraySegment[byte]]::new($receiveBuffer)
                try {
                    $result = $socket.ReceiveAsync(
                        $segment, $receiveCts.Token
                    ).GetAwaiter().GetResult()
                } catch [System.OperationCanceledException] {
                    $result = $null
                    break
                } catch [System.Management.Automation.MethodInvocationException] {
                    if ($_.Exception.InnerException -is
                        [System.OperationCanceledException]) {
                        $result = $null
                        break
                    }
                    throw
                }
            } finally {
                $receiveCts.Dispose()
            }
            if ($result.MessageType -eq
                [System.Net.WebSockets.WebSocketMessageType]::Close) {
                throw "Server closed WebSocket: $($socket.CloseStatus)"
            }
            if ($result.Count -gt 0) {
                $message.Write($receiveBuffer, 0, $result.Count)
            }
        } while ($null -ne $result -and -not $result.EndOfMessage)

        if ($null -eq $result) {
            $message.Dispose()
            break
        }
        if ($message.Length -eq 0) {
            $message.Dispose()
            continue
        }
        if ($result.MessageType -ne
            [System.Net.WebSockets.WebSocketMessageType]::Binary) {
            $message.Dispose()
            continue
        }

        $wire = $message.ToArray()
        $message.Dispose()
        $valid = $wire.Length -ge 32 -and
                 $wire[0] -eq 0x53 -and $wire[1] -eq 0x33 -and
                 $wire[2] -eq 0x4D -and $wire[3] -eq 0x44 -and
                 $wire[4] -eq 1
        if (-not $valid) {
            $invalid++
            continue
        }
        $sequence = Read-Be32 $wire 8
        $payloadLength = Read-Be32 $wire 20
        if ($wire.Length -ne 32 + $payloadLength) {
            $invalid++
            continue
        }
        if ($Kind -eq "video") {
            $valid = $wire[5] -eq 1 -and $payloadLength -le 524288 -and
                     $payloadLength -ge 4 -and
                     $wire[32] -eq 0xFF -and $wire[33] -eq 0xD8 -and
                     $wire[$wire.Length - 2] -eq 0xFF -and
                     $wire[$wire.Length - 1] -eq 0xD9
            $width = Read-Be16 $wire 24
            $height = Read-Be16 $wire 26
        } else {
            $valid = $wire[5] -eq 2 -and $payloadLength -eq 640 -and
                     (Read-Be16 $wire 24) -eq 16000 -and
                     (Read-Be16 $wire 26) -eq 320 -and
                     (Read-Be16 $wire 28) -eq 1 -and
                     (Read-Be16 $wire 30) -eq 16
            $sampleRate = Read-Be16 $wire 24
        }
        if (-not $valid) {
            $invalid++
            continue
        }
        if ($Kind -eq "video" -and
            -not [string]::IsNullOrWhiteSpace($SaveLatestJpeg) -and
            $width -eq 640 -and $height -eq 360) {
            $payloadBytes = [int]$payloadLength
            $latestJpeg = [byte[]]::new($payloadBytes)
            [Array]::Copy($wire, 32, $latestJpeg, 0, $payloadBytes)
        }

        if ($null -ne $lastSequence -and
            $sequence -ne (($lastSequence + 1) -band 0xFFFFFFFFL)) {
            $delta = ([int64]$sequence - [int64]$lastSequence - 1)
            if ($delta -gt 0) {
                $gaps += $delta
            } else {
                $gaps++
            }
        }
        $now = [DateTime]::UtcNow
        if ($null -eq $firstFrameAt) {
            $firstFrameAt = $now
            $firstSequence = $sequence
        }
        $lastFrameAt = $now
        $lastSequence = $sequence
        $frames++
        $bytes += $wire.Length
    }
} finally {
    if ($socket.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
        $closeCts = [Threading.CancellationTokenSource]::new(
            [TimeSpan]::FromSeconds(2)
        )
        try {
            $socket.CloseAsync(
                [System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure,
                "smoke test complete",
                $closeCts.Token
            ).GetAwaiter().GetResult() | Out-Null
        } catch {
            $socket.Abort()
        } finally {
            $closeCts.Dispose()
        }
    }
    $socket.Dispose()
    $connectCts.Dispose()
}

$observedSeconds = if ($frames -gt 1) {
    ($lastFrameAt - $firstFrameAt).TotalSeconds
} else {
    0
}
$fps = if ($observedSeconds -gt 0) {
    ($frames - 1) / $observedSeconds
} else {
    0
}

if ($null -ne $latestJpeg) {
    [IO.File]::WriteAllBytes(
        [IO.Path]::GetFullPath($SaveLatestJpeg),
        $latestJpeg
    )
}

[ordered]@{
    kind = $Kind
    device_id = $DeviceId
    frames = $frames
    invalid_frames = $invalid
    sequence_gaps = $gaps
    first_sequence = $firstSequence
    last_sequence = $lastSequence
    fps = [Math]::Round($fps, 2)
    wire_bytes = $bytes
    width = $width
    height = $height
    sample_rate = $sampleRate
} | ConvertTo-Json -Compress

if ($frames -eq 0 -or $invalid -ne 0) {
    exit 1
}
