<#
.SYNOPSIS
  Build muxaudio from the pinned git submodule and stage it for sapicli.

.DESCRIPTION
  Replaces fetch-muxaudio.ps1's "download a numbered release" step with a
  from-source build of the `extern/muxaudio` submodule. Produces the SAME
  staged layout the linker/loader expect, so sapicli.vcxproj needs no other
  change:

    Destination\include\mux.h                (platform-independent header)
    Destination\<Platform>\lib\muxaudio.lib  (import lib for the linker)
    Destination\<Platform>\bin\*.dll         (muxaudio.dll + codec DLLs)

  muxaudio's own codec dependencies (ogg/vorbis/opus/flac/lame/mpg123/...) are
  resolved by vcpkg in manifest mode; vcpkg's app-local deployment drops the
  dependent DLLs next to muxaudio.dll in the CMake build output, which we then
  sweep into bin\.

  Idempotent: skips the build when the submodule HEAD hasn't moved since the
  last successful stage (tracked in a stamp file), unless -Force.

.PARAMETER Platform
  MSBuild platform name: "Win32" (x86) or "x64".

.PARAMETER Destination
  Directory to stage into (layout above). Default: ..\third_party\muxaudio.

.PARAMETER Configuration
  CMake build configuration. Default: Release.

.PARAMETER Force
  Rebuild and re-stage even if the stamp says nothing changed.
#>
[CmdletBinding()]
param(
  [ValidateSet('Win32', 'x64')]
  [string] $Platform = 'Win32',
  [string] $Destination = (Join-Path $PSScriptRoot '..\third_party\muxaudio'),
  [string] $Configuration = 'Release',
  [switch] $Force
)

$ErrorActionPreference = 'Stop'

$repoRoot  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$srcDir    = Join-Path $repoRoot 'extern\muxaudio'

# MSBuild platform -> CMake arch + vcpkg triplet.
$arch    = if ($Platform -eq 'Win32') { 'Win32' } else { 'x64' }
$triplet = if ($Platform -eq 'Win32') { 'x86-windows' } else { 'x64-windows' }

$destInc = Join-Path $Destination 'include'
$destLib = Join-Path $Destination "$Platform\lib"
$destBin = Join-Path $Destination "$Platform\bin"
$stamp   = Join-Path $Destination "$Platform\.muxaudio.stamp"

# --- Ensure the submodule is checked out ---------------------------------
if (-not (Test-Path (Join-Path $srcDir 'CMakeLists.txt'))) {
  Write-Host "muxaudio submodule missing; initializing..."
  & git -C $repoRoot submodule update --init --recursive -- extern/muxaudio
  if ($LASTEXITCODE -ne 0) { throw "git submodule update failed ($LASTEXITCODE)" }
}

$head = (& git -C $srcDir rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "cannot read muxaudio submodule HEAD" }

# --- Idempotency: skip when nothing changed ------------------------------
if (-not $Force -and (Test-Path $stamp) -and (Test-Path (Join-Path $destBin 'muxaudio.dll'))) {
  if ((Get-Content $stamp -Raw).Trim() -eq $head) {
    Write-Host "muxaudio $head ($Platform) already staged; skipping build."
    exit 0
  }
}

# --- Locate vcpkg toolchain ----------------------------------------------
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
  $vc = Get-Command vcpkg -ErrorAction SilentlyContinue
  if ($vc) { $vcpkgRoot = Split-Path $vc.Source }
  elseif (Test-Path 'C:\vcpkg\scripts\buildsystems\vcpkg.cmake') { $vcpkgRoot = 'C:\vcpkg' }
}
if (-not $vcpkgRoot) { throw "vcpkg not found. Set VCPKG_ROOT or install vcpkg at C:\vcpkg." }
$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) { throw "vcpkg toolchain not found at $toolchain" }

# --- Configure + build ----------------------------------------------------
$buildDir = Join-Path $repoRoot "build\muxaudio-$Platform"
Write-Host "Building muxaudio $head ($Platform, $triplet) ..."

& cmake -S $srcDir -B $buildDir -G 'Visual Studio 17 2022' -A $arch `
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DVCPKG_TARGET_TRIPLET=$triplet" `
  -DBUILD_SHARED=ON -DBUILD_STATIC_FULL=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF `
  -DCODEC_VORBIS=ON -DCODEC_OPUS=ON -DCODEC_MP3=ON -DCODEC_FLAC=ON -DCODEC_AAC=ON
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

& cmake --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

# CMake/VS puts artifacts under $buildDir\$Configuration.
$outDir = Join-Path $buildDir $Configuration
$dll = Join-Path $outDir 'muxaudio.dll'
$lib = Join-Path $outDir 'muxaudio.lib'
if (-not (Test-Path $dll)) { throw "muxaudio.dll not found at $dll after build" }
if (-not (Test-Path $lib)) { throw "muxaudio.lib not found at $lib after build" }

# --- Stage ----------------------------------------------------------------
New-Item -ItemType Directory -Path $destInc -Force | Out-Null
New-Item -ItemType Directory -Path $destLib -Force | Out-Null
New-Item -ItemType Directory -Path $destBin -Force | Out-Null

Copy-Item (Join-Path $srcDir 'include\mux.h') (Join-Path $destInc 'mux.h') -Force
Copy-Item $lib (Join-Path $destLib 'muxaudio.lib') -Force

# Sweep muxaudio.dll + every dependent codec DLL vcpkg staged next to it.
Get-ChildItem -Path $outDir -Filter *.dll | ForEach-Object {
  Copy-Item $_.FullName (Join-Path $destBin $_.Name) -Force
}

Set-Content -Path $stamp -Value $head -NoNewline -Encoding ascii
Write-Host "Staged muxaudio $head ($Platform) at $Destination"
$dllNames = (Get-ChildItem $destBin -Filter *.dll | ForEach-Object Name) -join ', '
Write-Host "  DLLs: $dllNames"
