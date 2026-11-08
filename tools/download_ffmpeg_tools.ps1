<#
.SYNOPSIS
Downloads the pinned GNU make and NASM binaries used by the FFmpeg build.

.DESCRIPTION
The FFmpeg build needs a native GNU make and NASM. This script downloads the
versions ImView builds with into 3rd\tools, verifies their SHA-256 hashes and
skips files that are already correct.

3rd\tools is ignored by Git; the binaries are build tools, not source files.
Run tools\build_ffmpeg.cmd normally to fetch them automatically, or run
tools\download_ffmpeg_tools.cmd to fetch them without starting a build.

.PARAMETER Proxy
Optional proxy URL passed to Invoke-WebRequest.

.PARAMETER Force
Replaces files even when their hashes already match.
#>
[CmdletBinding()]
param(
    [string]$Proxy = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $root '3rd\tools'
New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null

$makeVersion = '4.4.1'
$makeUrl = 'https://raw.githubusercontent.com/mbuilov/gnumake-windows/2a2989816aece7c191c0015bff30d104ef31f52a/gnumake-4.4.1-x64.exe'
$makeHash = '368df1dcb3d768cda767809c21e4084c3398c9c9817f5819a8851e95782b44a5'
$makePath = Join-Path $toolsDir 'gnumake-4.4.1-x64.exe'

$nasmVersion = '3.02'
$nasmUrl = 'https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/nasm-3.02-win64.zip'
$nasmArchiveHash = '161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7'
$nasmExeHash = '04ec2385879f7e1c45dbe76c4020970555de48eeb97c23f59620ede061328f51'
$nasmPath = Join-Path $toolsDir 'nasm.exe'

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-Sha256 {
    param([string]$Path, [string]$Hash)
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and
           ((Get-Sha256 -Path $Path) -eq $Hash)
}

function Get-RemoteFile {
    param([string]$Uri, [string]$OutFile)
    $request = @{
        Uri = $Uri
        OutFile = $OutFile
        UseBasicParsing = $true
    }
    if ($Proxy) { $request.Proxy = $Proxy }
    Invoke-WebRequest @request
}

function Install-GnuMake {
    if (-not $Force -and (Test-Sha256 -Path $makePath -Hash $makeHash)) {
        Write-Host "GNU make $makeVersion already present: $makePath"
        return
    }
    $temp = Join-Path ([IO.Path]::GetTempPath()) (
        'imview-gnumake-' + [Guid]::NewGuid().ToString('N') + '.exe')
    try {
        Write-Host "Downloading GNU make $makeVersion x64..."
        Get-RemoteFile -Uri $makeUrl -OutFile $temp
        $actual = Get-Sha256 -Path $temp
        if ($actual -ne $makeHash) {
            throw "GNU make checksum mismatch.`n  expected: $makeHash`n  actual:   $actual"
        }
        Copy-Item -LiteralPath $temp -Destination $makePath -Force
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
    }
}

function Install-Nasm {
    if (-not $Force -and (Test-Sha256 -Path $nasmPath -Hash $nasmExeHash)) {
        Write-Host "NASM $nasmVersion already present: $nasmPath"
        return
    }
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'imview-nasm-' + [Guid]::NewGuid().ToString('N'))
    $archive = Join-Path $tempRoot 'nasm.zip'
    $extracted = Join-Path $tempRoot 'extracted'
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    try {
        Write-Host "Downloading NASM $nasmVersion x64..."
        Get-RemoteFile -Uri $nasmUrl -OutFile $archive
        $actualArchive = Get-Sha256 -Path $archive
        if ($actualArchive -ne $nasmArchiveHash) {
            throw "NASM archive checksum mismatch.`n  expected: $nasmArchiveHash`n  actual:   $actualArchive"
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $extracted -Force
        $exe = Get-ChildItem -LiteralPath $extracted -Filter 'nasm.exe' -Recurse -File |
               Select-Object -First 1
        if ($null -eq $exe) {
            throw "nasm.exe was not found in $nasmUrl"
        }
        $actualExe = Get-Sha256 -Path $exe.FullName
        if ($actualExe -ne $nasmExeHash) {
            throw "NASM executable checksum mismatch.`n  expected: $nasmExeHash`n  actual:   $actualExe"
        }
        Copy-Item -LiteralPath $exe.FullName -Destination $nasmPath -Force
    } finally {
        if (Test-Path -LiteralPath $tempRoot) {
            Remove-Item -LiteralPath $tempRoot -Recurse -Force
        }
    }
}

Install-GnuMake
Install-Nasm
Write-Host ''
Write-Host "FFmpeg build tools are ready in $toolsDir"
