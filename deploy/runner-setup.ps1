<#
.SYNOPSIS
  Install and register the Gitea Actions runner (`act_runner`) on this machine
  as a Windows service.

.DESCRIPTION
  One-shot setup. Downloads act_runner from gitea.com releases, registers it
  against the Gitea instance, and installs it as an auto-start Windows service
  running under the current user account (so it inherits PATH including
  vcpkg, VS Build Tools, emsdk, Python, etc.).

  Re-running with the same parameters is a no-op unless -Force is given.

.PARAMETER GiteaUrl
  Base URL of the Gitea instance, e.g. https://git.vampi.tech

.PARAMETER Token
  Runner registration token. In Gitea: Settings → Actions → Runners →
  "Create new runner" gives you the token.

.PARAMETER Name
  Friendly name for this runner (default: COMPUTERNAME).

.PARAMETER Labels
  Comma-separated runner labels (default: "self-hosted,windows,x64").
  Workflows pick the runner via these labels.

.PARAMETER InstallDir
  Where to install act_runner (default: C:\actrunner).

.PARAMETER Force
  Reinstall even if already present.

.EXAMPLE
  .\runner-setup.ps1 -GiteaUrl https://git.vampi.tech -Token <token>
#>

[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string] $GiteaUrl,
  [Parameter(Mandatory)] [string] $Token,
  [string] $Name    = $env:COMPUTERNAME,
  [string] $Labels  = "self-hosted,windows,x64",
  [string] $InstallDir = "C:\actrunner",
  [switch] $Force
)

$ErrorActionPreference = "Stop"

# act_runner releases are tagged like v0.2.x. Pin a known-good version.
$Version = "0.2.13"
$Url     = "https://gitea.com/gitea/act_runner/releases/download/v$Version/act_runner-$Version-windows-amd64.exe"
$Exe     = Join-Path $InstallDir "act_runner.exe"
$ConfigYaml = Join-Path $InstallDir "config.yaml"

function Test-Admin {
  $current = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal] $current).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) { throw "This script must be run as Administrator." }

# ---- prerequisites that the build workflow needs on PATH ----
# vcpkg, VS Build Tools (with ATL), and emsdk are expected at C:\vcpkg,
# C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools, and
# C:\emsdk respectively (set up earlier by hand). cmake / ninja / python
# go on PATH via winget so the workflow doesn't have to hunt for them.
# NSSM wraps act_runner.exe as a real Windows service (act_runner itself
# isn't SCM-aware so sc.exe / its own `service install` don't work).
$Pkgs = @(
  "Kitware.CMake",
  "Ninja-build.Ninja",
  "Python.Python.3.12",
  "NSSM.NSSM",
  # Node is needed by act_runner itself: every Gitea/GitHub Actions
  # JavaScript action (checkout@v4, upload-artifact@v3, etc.) is run
  # via `node` on the host. Without it act_runner fails the very first
  # step with "Cannot find: node in PATH".
  "OpenJS.NodeJS.LTS"
  # NOTE: PowerShell 7 is NOT in this list. winget's Microsoft.PowerShell
  # package installs user-scope by default (an app alias under
  # %LOCALAPPDATA%\Microsoft\WindowsApps\) which the LocalSystem service
  # can't see, and the package errors out on `--scope machine` with
  # "current system configuration does not support the installation".
  # Install from the upstream MSI a few lines down instead.
)
foreach ($p in $Pkgs) {
  Write-Host "winget install $p (no-op if already present)..."
  # --scope machine: actrunner runs as LocalSystem (or any service
  # account, really), so user-scope installs are invisible to it.
  & winget install --id $p -e --scope machine --accept-source-agreements --accept-package-agreements --silent | Out-Null
}

# PowerShell 7 from the upstream MSI -- see comment in $Pkgs above.
# ADD_PATH=1 puts pwsh.exe on machine PATH so the LocalSystem service
# can find it for `shell: pwsh` workflow steps.
$PwshExe = "C:\Program Files\PowerShell\7\pwsh.exe"
if (-not (Test-Path $PwshExe) -or $Force) {
  $PwshUrl = "https://github.com/PowerShell/PowerShell/releases/download/v7.5.3/PowerShell-7.5.3-win-x64.msi"
  $PwshMsi = Join-Path $env:TEMP "pwsh-7.5.3-x64.msi"
  Write-Host "Downloading PowerShell 7.5.3..."
  Invoke-WebRequest -Uri $PwshUrl -OutFile $PwshMsi
  Write-Host "Installing PowerShell 7.5.3 (machine-wide, ADD_PATH=1)..."
  & msiexec /i $PwshMsi /quiet /norestart ADD_PATH=1 | Out-Null
}

if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory $InstallDir | Out-Null }

if (-not (Test-Path $Exe) -or $Force) {
  Write-Host "Downloading act_runner v$Version..."
  Invoke-WebRequest -Uri $Url -OutFile $Exe
}

# Write a minimal config.yaml. We can't use `act_runner generate-config`
# because that emits the upstream example which pins ubuntu Docker labels
# and is full of comments; cleaner to hand-write the few keys we care
# about. Labels MUST live here and not on the `register` CLI -- v0.2.13
# silently ignores --labels and uses whatever's in this file at register
# time, which then gets baked into .runner.
if (-not (Test-Path $ConfigYaml) -or $Force) {
  $labelLines = ($Labels -split ',' | ForEach-Object { "    - `"$($_.Trim())`"" }) -join "`n"
  $cfg = @"
log:
  level: info

runner:
  file: .runner
  capacity: 1
  timeout: 3h
  insecure: false
  fetch_timeout: 5s
  fetch_interval: 2s
  labels:
$labelLines

cache:
  enabled: true

container:
  network: ""
  privileged: false

host:
  workdir_parent: ""
"@
  Set-Content -Path $ConfigYaml -Value $cfg -Encoding utf8
}

# Register against the Gitea instance. This writes .runner alongside config.yaml.
$RunnerFile = Join-Path $InstallDir ".runner"
if ((-not (Test-Path $RunnerFile)) -or $Force) {
  if (Test-Path $RunnerFile) { Remove-Item $RunnerFile -Force }
  Write-Host "Registering runner '$Name' against $GiteaUrl..."
  Push-Location $InstallDir
  try {
    & $Exe register --no-interactive --instance $GiteaUrl --token $Token `
                    --name $Name --config $ConfigYaml
    if ($LASTEXITCODE -ne 0) { throw "act_runner register failed (exit $LASTEXITCODE)" }
  } finally { Pop-Location }
}

# Install as a Windows service via NSSM. We run as LocalSystem (NSSM's
# default), not as the current user, so we don't have to handle a
# password. The build workflow sets VCPKG_ROOT explicitly and shells out
# to vcvars64.bat / emsdk_env.bat by absolute path, so it doesn't depend
# on the user's PATH -- only on cmake/ninja/python being on machine PATH
# (winget puts them there with --scope machine, but its default scope
# is good enough since the service inherits the machine PATH).
$ServiceName = "actrunner"

# Find nssm.exe. Order of preference:
#   1. $InstallDir\nssm.exe (copied here by an earlier run)
#   2. on PATH (will work in a fresh shell after winget install)
#   3. directly in winget's package cache (works in the current shell
#      immediately after the winget install above, since winget hasn't
#      flushed PATH into our env block yet)
# We then copy whatever we found into $InstallDir so subsequent runs
# don't have to hunt.
$NssmLocal = Join-Path $InstallDir "nssm.exe"
$Nssm = $null
if (Test-Path $NssmLocal) {
  $Nssm = $NssmLocal
} else {
  $cmd = Get-Command nssm.exe -ErrorAction SilentlyContinue
  if ($cmd) { $Nssm = $cmd.Source }
}
if (-not $Nssm) {
  $candidates = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages","C:\Program Files\NSSM","C:\Program Files (x86)\NSSM" `
                              -Filter nssm.exe -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match 'win64' } |
                Select-Object -First 1
  if ($candidates) { $Nssm = $candidates.FullName }
}
if (-not $Nssm) { throw "nssm.exe not found. Run 'winget install NSSM.NSSM' manually and re-run." }
if ($Nssm -ne $NssmLocal) {
  Copy-Item $Nssm $NssmLocal -Force
  $Nssm = $NssmLocal
}

$Existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Existing -and $Force) {
  Write-Host "Stopping + removing existing service..."
  Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
  & $Nssm remove $ServiceName confirm | Out-Null
  $Existing = $null
}
if (-not $Existing) {
  Write-Host "Installing service '$ServiceName' via NSSM..."
  & $Nssm install $ServiceName $Exe daemon -c $ConfigYaml | Out-Null
  & $Nssm set $ServiceName AppDirectory $InstallDir | Out-Null
  & $Nssm set $ServiceName Start         SERVICE_AUTO_START | Out-Null
  & $Nssm set $ServiceName AppStdout     (Join-Path $InstallDir "stdout.log") | Out-Null
  & $Nssm set $ServiceName AppStderr     (Join-Path $InstallDir "stderr.log") | Out-Null
  & $Nssm set $ServiceName Description   "Gitea Actions runner (act_runner) for $GiteaUrl" | Out-Null
  Start-Service $ServiceName
}

Write-Host ""
Write-Host "Done. Runner '$Name' is registered and running."
Write-Host "  Install dir : $InstallDir"
Write-Host "  Service     : $ServiceName"
Write-Host "  Logs        : Get-Content $InstallDir\stdout.log -Wait"
