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
$Pkgs = @("Kitware.CMake", "Ninja-build.Ninja", "Python.Python.3.12")
foreach ($p in $Pkgs) {
  Write-Host "winget install $p (no-op if already present)..."
  & winget install --id $p -e --accept-source-agreements --accept-package-agreements --silent | Out-Null
}

if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory $InstallDir | Out-Null }

if (-not (Test-Path $Exe) -or $Force) {
  Write-Host "Downloading act_runner v$Version..."
  Invoke-WebRequest -Uri $Url -OutFile $Exe
}

# Generate a default config if missing (we keep most defaults).
if (-not (Test-Path $ConfigYaml) -or $Force) {
  Push-Location $InstallDir
  try { & $Exe generate-config | Out-File -Encoding utf8 $ConfigYaml }
  finally { Pop-Location }
}

# Register against the Gitea instance. This writes .runner alongside config.yaml.
$RunnerFile = Join-Path $InstallDir ".runner"
if (-not (Test-Path $RunnerFile) -or $Force) {
  Write-Host "Registering runner '$Name' against $GiteaUrl..."
  Push-Location $InstallDir
  try {
    & $Exe register --no-interactive --instance $GiteaUrl --token $Token `
                    --name $Name --labels $Labels --config $ConfigYaml
  } finally { Pop-Location }
}

# Install as a Windows service. act_runner's `service install` registers the
# service under the current user, which is what we want — the build needs the
# user's PATH (vcpkg, vs build tools, emsdk, python).
$ServiceName = "actrunner"
$Existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Existing -and $Force) {
  Write-Host "Stopping + removing existing service..."
  Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
  & sc.exe delete $ServiceName | Out-Null
  $Existing = $null
}
if (-not $Existing) {
  Write-Host "Installing service '$ServiceName'..."
  Push-Location $InstallDir
  try {
    & $Exe service install --user --config $ConfigYaml
  } finally { Pop-Location }
  Start-Service $ServiceName
}

Write-Host ""
Write-Host "Done. Runner '$Name' is registered and running."
Write-Host "  Install dir : $InstallDir"
Write-Host "  Service     : $ServiceName"
Write-Host "  Logs        : Get-Service $ServiceName ; Get-EventLog Application -Source actrunner -Newest 20"
