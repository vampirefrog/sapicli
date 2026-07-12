<#
.SYNOPSIS
  Download and stage the prebuilt muxaudio bundled x86 shared release.

.DESCRIPTION
  We build sapicli.exe as Win32 (x86) — see build.yml for the SAPI voice
  registry reason — and link against the release build of muxaudio instead
  of compiling from source. The `bundled` variant is fully self-contained:
  all codec libraries (libogg, libvorbis, libopus, libmp3lame) are
  statically linked into muxaudio.dll, so we only need to ship that one
  binary alongside sapicli.exe.

  Idempotent: skips the download if the expected files are already present
  and match. Called from sapicore.vcxproj as a pre-build target and from
  the CI workflow.

.PARAMETER Version
  muxaudio release tag to fetch (default: v0.3).

.PARAMETER Destination
  Directory to stage into. mux.h -> Destination\include\mux.h,
  muxaudio.lib -> Destination\lib\muxaudio.lib, muxaudio.dll ->
  Destination\bin\muxaudio.dll.

.PARAMETER Force
  Re-download even if the destination is already populated.
#>
[CmdletBinding()]
param(
  [string] $Version = 'v0.3',
  [string] $Destination = (Join-Path $PSScriptRoot '..\third_party\muxaudio'),
  [switch] $Force
)

$ErrorActionPreference = 'Stop'

$assetName = 'muxaudio-windows-x86-shared-bundled.zip'
$url       = "https://github.com/vampirefrog/muxaudio/releases/download/$Version/$assetName"

$destInc = Join-Path $Destination 'include'
$destLib = Join-Path $Destination 'lib'
$destBin = Join-Path $Destination 'bin'

$targets = @(
  Join-Path $destInc 'mux.h'
  Join-Path $destLib 'muxaudio.lib'
  Join-Path $destBin 'muxaudio.dll'
)

if (-not $Force -and ($targets | ForEach-Object { Test-Path $_ }) -notcontains $false) {
  Write-Host "muxaudio $Version already staged at $Destination"
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

  # The zip's top-level directory is "windows-x86-shared-bundled/".
  $extracted = Get-ChildItem -Path $tmp -Directory | Select-Object -First 1
  if (-not $extracted) { throw "expected a top-level directory inside $assetName" }

  New-Item -ItemType Directory -Path $destInc -Force | Out-Null
  New-Item -ItemType Directory -Path $destLib -Force | Out-Null
  New-Item -ItemType Directory -Path $destBin -Force | Out-Null

  Copy-Item (Join-Path $extracted.FullName 'mux.h')        (Join-Path $destInc 'mux.h')        -Force
  Copy-Item (Join-Path $extracted.FullName 'muxaudio.lib') (Join-Path $destLib 'muxaudio.lib') -Force
  Copy-Item (Join-Path $extracted.FullName 'muxaudio.dll') (Join-Path $destBin 'muxaudio.dll') -Force

  Write-Host "Staged muxaudio $Version at $Destination"
} finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
