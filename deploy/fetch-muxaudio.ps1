<#
.SYNOPSIS
  Download and stage the prebuilt muxaudio shared release for a given platform.

.DESCRIPTION
  We link against muxaudio.lib and ship the muxaudio.dll (and its codec-DLL
  dependencies: ogg / vorbis / opus / mpg123 / FLAC / fdk-aac / ...) alongside
  sapicli.exe. This script downloads the platform-appropriate archive from
  the muxaudio GitHub release and stages it into:

    Destination\include\mux.h                (platform-independent header)
    Destination\<Platform>\lib\muxaudio.lib  (import lib for the linker)
    Destination\<Platform>\bin\*.dll         (runtime DLLs for the loader)

  Idempotent: skips the download if the expected paths for <Platform> are
  already present. Called from sapicli.vcxproj as a pre-build target and
  from the CI workflow.

.PARAMETER Platform
  MSBuild platform name: "Win32" (x86) or "x64". Maps to the corresponding
  muxaudio-windows-{x86,x64}-shared.zip release asset.

.PARAMETER Version
  muxaudio release tag to fetch (default: v0.4).

.PARAMETER Destination
  Directory to stage into. Layout above.

.PARAMETER Force
  Re-download even if the destination is already populated.
#>
[CmdletBinding()]
param(
  [ValidateSet('Win32', 'x64')]
  [string] $Platform = 'Win32',
  [string] $Version  = 'v0.4',
  [string] $Destination = (Join-Path $PSScriptRoot '..\third_party\muxaudio'),
  [switch] $Force
)

$ErrorActionPreference = 'Stop'

# Map MSBuild's platform names to muxaudio's release-asset naming.
$arch = if ($Platform -eq 'Win32') { 'x86' } else { 'x64' }
$assetName = "muxaudio-windows-$arch-shared.zip"
$url       = "https://github.com/vampirefrog/muxaudio/releases/download/$Version/$assetName"

$destInc = Join-Path $Destination 'include'
$destLib = Join-Path $Destination "$Platform\lib"
$destBin = Join-Path $Destination "$Platform\bin"

# Presence check: the header at include/, the import lib under <Platform>/lib,
# and at least the main DLL under <Platform>/bin. Codec DLLs get overwritten
# each time we stage; we don't try to enumerate them here.
$sentinels = @(
  Join-Path $destInc 'mux.h'
  Join-Path $destLib 'muxaudio.lib'
  Join-Path $destBin 'muxaudio.dll'
)

if (-not $Force -and ($sentinels | ForEach-Object { Test-Path $_ }) -notcontains $false) {
  Write-Host "muxaudio $Version ($Platform) already staged at $Destination"
  exit 0
}

$tmp = Join-Path $env:TEMP "fetch-muxaudio-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
  $zip = Join-Path $tmp $assetName
  Write-Host "Downloading $url ..."
  Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

  Write-Host "Extracting ..."
  Expand-Archive -Path $zip -DestinationPath $tmp -Force

  # The zip's top-level directory is "windows-<arch>-shared/".
  $extracted = Get-ChildItem -Path $tmp -Directory | Select-Object -First 1
  if (-not $extracted) { throw "expected a top-level directory inside $assetName" }

  New-Item -ItemType Directory -Path $destInc -Force | Out-Null
  New-Item -ItemType Directory -Path $destLib -Force | Out-Null
  New-Item -ItemType Directory -Path $destBin -Force | Out-Null

  Copy-Item (Join-Path $extracted.FullName 'mux.h')        (Join-Path $destInc 'mux.h')        -Force
  Copy-Item (Join-Path $extracted.FullName 'muxaudio.lib') (Join-Path $destLib 'muxaudio.lib') -Force

  # Sweep every DLL from the shared bundle into <Platform>/bin. sapicli's
  # post-build copies the lot to $(OutDir), which mirrors what the release
  # zip ships.
  Get-ChildItem -Path $extracted.FullName -Filter *.dll | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $destBin $_.Name) -Force
  }

  Write-Host "Staged muxaudio $Version ($Platform) at $Destination"
} finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
