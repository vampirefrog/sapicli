<#
.SYNOPSIS
  Push a sapisrv zip artifact to a remote Windows machine via PSRemoting.

.DESCRIPTION
  End-to-end remote deploy:

    1.  Open a PSSession to -ComputerName.
    2.  Stop the sapisrv service if it's running.
    3.  Copy the local zip to the remote temp dir.
    4.  Expand the zip into -InstallDir on the remote (overwriting binaries
        and the bundled www/ tree).
    5.  First-deploy bootstrap (if the service doesn't exist yet):
          - sc create sapisrv with the right binPath and account
          - netsh http add urlacl for the chosen port
          - Generate %ProgramData%\sapicli\keys.json with a random
            public-trial key (10 req/min + 100 req/hour per IP)
    6.  Start the service, wait for RUNNING, then GET /health to confirm
        the HTTP listener is actually up.

  All remote work runs as the connecting user (must be a local Administrator
  on the target). Idempotent: re-running with a newer zip just swaps files
  and bounces the service.

.PARAMETER ComputerName
  DNS name or IP of the target Windows machine. Must have PSRemoting
  enabled (`Enable-PSRemoting -Force` if not).

.PARAMETER ZipPath
  Path to the sapisrv-<version>-<sha>.zip built by CI.

.PARAMETER Credential
  Optional PSCredential for the remote machine. If omitted, the current
  user's credentials are used (works for domain-joined boxes).

.PARAMETER InstallDir
  Where to unzip on the remote. Default: C:\Program Files\sapisrv

.PARAMETER Port
  TCP port for the HTTP listener. Default: 8080. Changing this on a
  redeploy: the script removes the old urlacl + service binPath and
  re-creates them so they match.

.PARAMETER HealthTimeoutSeconds
  How long to wait for the /health endpoint to start responding after
  starting the service. Default: 30.

.EXAMPLE
  .\deploy\Deploy-Remote.ps1 -ComputerName prod-vm `
    -ZipPath .\sapisrv-0.1.2.zip

.EXAMPLE
  .\deploy\Deploy-Remote.ps1 -ComputerName 10.0.0.5 `
    -ZipPath .\sapisrv-0.1.2.zip `
    -Credential (Get-Credential) -Port 9090
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string] $ComputerName,
  [Parameter(Mandatory)] [string] $ZipPath,
  [PSCredential] $Credential,
  [string] $InstallDir = 'C:\Program Files\sapisrv',
  [int]    $Port = 8080,
  [int]    $HealthTimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $ZipPath)) { throw "Zip not found: $ZipPath" }
$ZipPath = (Resolve-Path $ZipPath).Path
$ZipName = Split-Path -Leaf $ZipPath

Write-Host "Connecting to $ComputerName..."
$sessionParams = @{ ComputerName = $ComputerName }
if ($Credential) { $sessionParams['Credential'] = $Credential }
$session = New-PSSession @sessionParams
try {
  $remoteZip = Invoke-Command -Session $session -ScriptBlock {
    param($name) Join-Path $env:TEMP $name
  } -ArgumentList $ZipName

  Write-Host "Copying $ZipName to ${ComputerName}:$remoteZip ($((Get-Item $ZipPath).Length) bytes)..."
  Copy-Item -Path $ZipPath -Destination $remoteZip -ToSession $session -Force

  Write-Host "Deploying on $ComputerName..."
  $result = Invoke-Command -Session $session -ScriptBlock {
    param($remoteZip, $InstallDir, $Port, $HealthTimeoutSeconds)
    $ErrorActionPreference = 'Stop'

    $svcName = 'sapisrv'
    $svc = Get-Service -Name $svcName -ErrorAction SilentlyContinue

    # ---- Stop the service so we can overwrite the exe ----
    if ($svc -and $svc.Status -ne 'Stopped') {
      Write-Host "  Stopping $svcName..."
      Stop-Service $svcName -Force
      $svc.WaitForStatus('Stopped', '00:00:30')
    }

    # ---- Unzip into $InstallDir ----
    if (-not (Test-Path $InstallDir)) {
      New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }
    Write-Host "  Expanding zip into $InstallDir..."
    Expand-Archive -Path $remoteZip -DestinationPath $InstallDir -Force
    Remove-Item $remoteZip -Force -ErrorAction SilentlyContinue

    $exe = Join-Path $InstallDir 'sapisrv.exe'
    if (-not (Test-Path $exe)) { throw "sapisrv.exe missing under $InstallDir after unzip" }

    # ---- First-deploy bootstrap ----
    $isFirstDeploy = $null -eq $svc

    # URL ACL: re-create so the port + identity are always current.
    Write-Host "  (Re)reserving URL ACL http://+:$Port/ for NetworkService..."
    & netsh http delete urlacl url="http://+:$Port/" 2>&1 | Out-Null
    & netsh http add urlacl url="http://+:$Port/" user="NT AUTHORITY\NetworkService" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "netsh http add urlacl failed (exit $LASTEXITCODE)" }

    # Service: re-create with current binPath + port arg so an upgrade
    # picks up any --port change. Use WMI's Win32_Service.Create rather
    # than sc.exe because sc.exe's binPath= eats embedded quotes badly
    # under PowerShell's native-arg quoting (the "C:\Program Files\..."
    # path needs quotes around it AND a space + flags after, which makes
    # PS double-escape the inner quotes and sc.exe vomits its USAGE).
    # WMI takes the path as a structural string parameter, no shell
    # parsing involved.
    $binPath = "`"$exe`" run --port=$Port"
    if ($isFirstDeploy) {
      Write-Host "  Creating service $svcName..."
    } else {
      Write-Host "  Recreating service $svcName (binPath / port may have changed)..."
      $existing = Get-WmiObject -Class Win32_Service -Filter "Name='$svcName'"
      if ($existing) { $existing.Delete() | Out-Null }
      Start-Sleep -Milliseconds 500
    }
    $svcWmi = [WMIClass]'Win32_Service'
    $r = $svcWmi.Create(
      $svcName,                                    # Name
      'sapicli SAPI HTTP Server',                  # DisplayName
      $binPath,                                    # PathName  (full quoted cmdline)
      16,                                          # ServiceType: 16 = OWN_PROCESS
      1,                                           # ErrorControl: 1 = NORMAL
      'Automatic',                                 # StartMode
      $false,                                      # DesktopInteract
      'NT AUTHORITY\NetworkService',               # StartName  (account)
      $null,                                       # StartPassword (built-in account, none)
      $null,                                       # LoadOrderGroup
      $null,                                       # LoadOrderGroupDependencies
      $null                                        # ServiceDependencies
    )
    if ($r.ReturnValue -ne 0) {
      throw "Win32_Service.Create returned $($r.ReturnValue) (see https://learn.microsoft.com/windows/win32/cimwin32prov/create-method-in-class-win32-service)"
    }
    # WMI doesn't expose Description; sc.exe handles a single string fine.
    & sc.exe description $svcName 'Serves SAPI text-to-speech over HTTP, with parallel speech-event multiplexing.' | Out-Null

    # keys.json: only generate on a fresh install. Operator-edited files
    # live forever.
    $keysDir  = Join-Path $env:ProgramData 'sapicli'
    $keysPath = Join-Path $keysDir 'keys.json'
    if (-not (Test-Path $keysDir)) { New-Item -ItemType Directory $keysDir -Force | Out-Null }
    if (-not (Test-Path $keysPath)) {
      Write-Host "  Generating fresh keys.json with random public-trial key..."
      $bytes = New-Object byte[] 24
      [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
      $key = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
      $cfg = @{
        public_tier = @{ qps = 0.5; burst = 3 }
        keys = @{
          $key = @{
            name    = 'public-trial'
            default = $true
            qps     = 2.0
            burst   = 10.0
            ip_limits = @(
              @{ requests = 10;  per_seconds = 60   },
              @{ requests = 100; per_seconds = 3600 }
            )
          }
        }
      }
      ($cfg | ConvertTo-Json -Depth 5) | Set-Content -Path $keysPath -Encoding utf8
    }

    # ---- Start + verify ----
    Write-Host "  Starting $svcName..."
    Start-Service $svcName
    (Get-Service $svcName).WaitForStatus('Running', '00:00:30')

    Write-Host "  Polling http://localhost:$Port/health (timeout ${HealthTimeoutSeconds}s)..."
    $deadline = (Get-Date).AddSeconds($HealthTimeoutSeconds)
    $health = $null
    while ((Get-Date) -lt $deadline) {
      try {
        $health = Invoke-RestMethod -Uri "http://localhost:$Port/health" -TimeoutSec 3
        break
      } catch {
        Start-Sleep -Milliseconds 500
      }
    }
    if (-not $health) { throw "service didn't answer /health within ${HealthTimeoutSeconds}s" }

    [pscustomobject]@{
      Hostname     = $env:COMPUTERNAME
      InstallDir   = $InstallDir
      Port         = $Port
      ServicePid   = $health.pid
      UptimeSec    = $health.uptime_s
      FirstDeploy  = $isFirstDeploy
    }
  } -ArgumentList $remoteZip, $InstallDir, $Port, $HealthTimeoutSeconds

  Write-Host ""
  Write-Host "Deployment OK." -ForegroundColor Green
  $result | Format-List
}
finally {
  Remove-PSSession $session -ErrorAction SilentlyContinue
}
