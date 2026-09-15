param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$expectedSize = 6135808
$expectedMd5 = 'C5C5DBED69AF10A43A6037FD312384F2'
$expectedSha256 = '36FB81DB38469BABF15E9EDFD40959CB22609DB9005A51D4250BBC86BBCF3DD9'
$canonicalSha256 = '941C755553B9D6B6C842AEBA8B7EB22600D828AF69F945B377F4146A833DC41D'
$expectedChecksum = 0x005DC744
$checksumOffset = 0x160
$verificationCode = 868086
$footerSize = 64

if (-not ('PeChecksum.NativeMethods' -as [type])) {
    Add-Type -TypeDefinition @'
namespace PeChecksum {
    using System.Runtime.InteropServices;

    public static class NativeMethods {
        [DllImport("imagehlp.dll", CharSet = CharSet.Unicode,
            SetLastError = true)]
        public static extern uint MapFileAndCheckSum(
            string filename, out uint headerSum, out uint checkSum);
    }
}
'@
}

$inputFile = Get-Item -LiteralPath $InputPath
$inputFullPath = $inputFile.FullName
$outputFullPath = [IO.Path]::GetFullPath($OutputPath)
if ([StringComparer]::OrdinalIgnoreCase.Equals($inputFullPath, $outputFullPath)) {
    throw 'InputPath and OutputPath must be different; this tool never stamps in place'
}
if ($inputFile.Length -ne $expectedSize) {
    throw "Unexpected input size $($inputFile.Length); expected $expectedSize"
}

$inputMd5 = (Get-FileHash -LiteralPath $inputFullPath -Algorithm MD5).Hash
if ($inputMd5 -ne $expectedMd5) {
    throw "Unexpected input MD5 $inputMd5; expected $expectedMd5"
}
$inputSha256 = (Get-FileHash -LiteralPath $inputFullPath -Algorithm SHA256).Hash
if ($inputSha256 -ne $expectedSha256) {
    throw "Unexpected input SHA256 $inputSha256; expected $expectedSha256"
}

$bytes = [IO.File]::ReadAllBytes($inputFullPath)
if ([BitConverter]::ToUInt32($bytes, $checksumOffset) -ne $expectedChecksum) {
    throw 'The original PE checksum field does not match the verified build'
}

$canonical = [byte[]]$bytes.Clone()
[Array]::Clear($canonical, $checksumOffset, 4)
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $canonicalActual = [BitConverter]::ToString(
        $sha.ComputeHash($canonical)).Replace('-', '')
} finally {
    $sha.Dispose()
}
if ($canonicalActual -ne $canonicalSha256) {
    throw "Unexpected canonical SHA256 $canonicalActual; expected $canonicalSha256"
}

$footer = New-Object byte[] $footerSize
$magic = [Text.Encoding]::ASCII.GetBytes('NFSMWRF1')
[Array]::Copy($magic, 0, $footer, 0, $magic.Length)
$fields = @(
    [uint32]1,
    [uint32]$footerSize,
    [uint32]$verificationCode,
    [uint32]$expectedSize,
    [uint32]$expectedChecksum,
    [uint32]1
)
for ($i = 0; $i -lt $fields.Count; ++$i) {
    $fieldBytes = [BitConverter]::GetBytes($fields[$i])
    [Array]::Copy($fieldBytes, 0, $footer, 8 + $i * 4, 4)
}
$canonicalDigest = for ($i = 0; $i -lt $canonicalSha256.Length; $i += 2) {
    [Convert]::ToByte($canonicalSha256.Substring($i, 2), 16)
}
[Array]::Copy([byte[]]$canonicalDigest, 0, $footer, 32, 32)

$stamped = New-Object byte[] ($bytes.Length + $footer.Length)
[Array]::Copy($bytes, 0, $stamped, 0, $bytes.Length)
[Array]::Copy($footer, 0, $stamped, $bytes.Length, $footer.Length)
$outputDirectory = [IO.Path]::GetDirectoryName($outputFullPath)
if (-not [String]::IsNullOrEmpty($outputDirectory)) {
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
}
[IO.File]::WriteAllBytes($outputFullPath, $stamped)

[uint32]$headerSum = 0
[uint32]$computedChecksum = 0
$checksumStatus = [PeChecksum.NativeMethods]::MapFileAndCheckSum(
    $outputFullPath, [ref]$headerSum, [ref]$computedChecksum)
if ($checksumStatus -ne 0) {
    throw "MapFileAndCheckSum failed with status $checksumStatus"
}
$checksumBytes = [BitConverter]::GetBytes($computedChecksum)
$stream = [IO.File]::Open($outputFullPath, [IO.FileMode]::Open,
    [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $stream.Position = $checksumOffset
    $stream.Write($checksumBytes, 0, $checksumBytes.Length)
    $stream.Flush($true)
} finally {
    $stream.Dispose()
}

[uint32]$verifiedHeaderSum = 0
[uint32]$verifiedChecksum = 0
$verifyStatus = [PeChecksum.NativeMethods]::MapFileAndCheckSum(
    $outputFullPath, [ref]$verifiedHeaderSum, [ref]$verifiedChecksum)
if ($verifyStatus -ne 0 -or $verifiedHeaderSum -ne $verifiedChecksum -or
    $verifiedChecksum -ne $computedChecksum) {
    throw 'The stamped executable PE checksum did not verify'
}

$output = Get-Item -LiteralPath $outputFullPath
[pscustomobject]@{
    Path = $output.FullName
    Size = $output.Length
    FooterOffset = ('0x{0:X}' -f $expectedSize)
    FooterSize = $footerSize
    VerificationCode = $verificationCode
    PEChecksum = ('0x{0:X8}' -f $computedChecksum)
    MD5 = (Get-FileHash -LiteralPath $output.FullName -Algorithm MD5).Hash
    SHA256 = (Get-FileHash -LiteralPath $output.FullName -Algorithm SHA256).Hash
}
