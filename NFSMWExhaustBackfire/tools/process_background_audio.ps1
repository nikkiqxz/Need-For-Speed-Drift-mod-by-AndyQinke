param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [Parameter(Mandatory = $true)]
    [string]$DestinationDirectory
)

$ErrorActionPreference = 'Stop'

function Convert-BackgroundWave {
    param(
        [string]$Source,
        [string]$Destination
    )

    $bytes = [IO.File]::ReadAllBytes($Source)
    if ($bytes.Length -lt 44 -or
        [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'RIFF' -or
        [Text.Encoding]::ASCII.GetString($bytes, 8, 4) -ne 'WAVE') {
        throw "$Source is not a RIFF/WAVE file"
    }

    $formatOffset = -1
    $dataOffset = -1
    $dataSize = 0
    $offset = 12
    while ($offset + 8 -le $bytes.Length) {
        $chunkId = [Text.Encoding]::ASCII.GetString($bytes, $offset, 4)
        $chunkSize = [BitConverter]::ToUInt32($bytes, $offset + 4)
        $payload = $offset + 8
        if ($payload + $chunkSize -gt $bytes.Length) {
            throw "$Source has a truncated WAV chunk"
        }
        if ($chunkId -eq 'fmt ') {
            $formatOffset = $payload
        } elseif ($chunkId -eq 'data') {
            $dataOffset = $payload
            $dataSize = $chunkSize
            break
        }
        $offset = $payload + $chunkSize + ($chunkSize % 2)
    }

    if ($formatOffset -lt 0 -or $dataOffset -lt 0) {
        throw "$Source is missing fmt or data"
    }
    $formatTag = [BitConverter]::ToUInt16($bytes, $formatOffset)
    $channels = [BitConverter]::ToUInt16($bytes, $formatOffset + 2)
    $sampleRate = [BitConverter]::ToUInt32($bytes, $formatOffset + 4)
    $bits = [BitConverter]::ToUInt16($bytes, $formatOffset + 14)
    if ($formatTag -ne 1 -or $channels -ne 2 -or $bits -ne 16) {
        throw "$Source must be stereo 16-bit PCM"
    }

    $frameCount = [int]($dataSize / 4)
    $mono = New-Object double[] $frameCount
    $mean = 0.0
    for ($frame = 0; $frame -lt $frameCount; ++$frame) {
        $left = [BitConverter]::ToInt16($bytes, $dataOffset + $frame * 4)
        $right = [BitConverter]::ToInt16($bytes, $dataOffset + $frame * 4 + 2)
        $mono[$frame] = ($left + $right) * 0.5
        $mean += $mono[$frame]
    }
    $mean /= [Math]::Max(1, $frameCount)

    $fadeInFrames = [Math]::Min($frameCount, [int][Math]::Round($sampleRate * 0.010))
    $fadeOutFrames = [Math]::Min($frameCount, [int][Math]::Round($sampleRate * 0.120))
    $peak = 0.0
    for ($frame = 0; $frame -lt $frameCount; ++$frame) {
        $sample = $mono[$frame] - $mean
        if ($fadeInFrames -gt 1 -and $frame -lt $fadeInFrames) {
            $sample *= [Math]::Sin(($frame / ($fadeInFrames - 1.0)) * [Math]::PI / 2.0)
        }
        if ($fadeOutFrames -gt 1 -and $frame -ge $frameCount - $fadeOutFrames) {
            $remaining = $frameCount - 1 - $frame
            $sample *= [Math]::Sin(($remaining / ($fadeOutFrames - 1.0)) * [Math]::PI / 2.0)
        }
        $mono[$frame] = $sample
        $peak = [Math]::Max($peak, [Math]::Abs($sample))
    }

    $targetPeak = 32767.0 * [Math]::Pow(10.0, -14.0 / 20.0)
    $scale = if ($peak -gt 0.0) { $targetPeak / $peak } else { 1.0 }
    $stream = [IO.File]::Open($Destination, [IO.FileMode]::Create)
    try {
        $writer = [IO.BinaryWriter]::new($stream)
        $dataBytes = $frameCount * 2
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $dataBytes))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
        $writer.Write([uint32]16)
        $writer.Write([uint16]1)
        $writer.Write([uint16]1)
        $writer.Write([uint32]$sampleRate)
        $writer.Write([uint32]($sampleRate * 2))
        $writer.Write([uint16]2)
        $writer.Write([uint16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([uint32]$dataBytes)
        foreach ($sample in $mono) {
            $value = [Math]::Max(-32768.0, [Math]::Min(32767.0, [Math]::Round($sample * $scale)))
            $writer.Write([int16]$value)
        }
        $writer.Flush()
    } finally {
        $stream.Dispose()
    }
}

New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
Convert-BackgroundWave -Source $InputPath -Destination (Join-Path $DestinationDirectory 'IN.wav')
Convert-BackgroundWave -Source $OutputPath -Destination (Join-Path $DestinationDirectory 'OUT.wav')
