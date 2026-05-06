<#
.SYNOPSIS
  Generate %ProgramData%\sapicli\keys.json on first install if missing.

.DESCRIPTION
  Idempotent. If -Path already exists, exits 0 without touching it (the
  operator may have edited it). Otherwise writes a fresh keys.json with
  one randomly-generated 24-byte url-safe-base64 public-trial key, marked
  default=true so the bundled web client picks it up via /api/default-key.
  Rate-limited 10 req/min and 100 req/hour per IP -- sized for a public
  demo. The key is *not* shown anywhere; sysadmins can read it out of
  the emitted keys.json. On uninstall the file is preserved.

.PARAMETER Path
  Where to write keys.json (default: %ProgramData%\sapicli\keys.json).
#>
[CmdletBinding()]
param(
  [string] $Path = (Join-Path $env:ProgramData 'sapicli\keys.json')
)

$ErrorActionPreference = 'Stop'

if (Test-Path $Path) {
  Write-Host "keys.json already exists at $Path -- leaving it alone."
  exit 0
}

$bytes = New-Object byte[] 24
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$key = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')

$cfg = @{
  public_tier = @{ qps = 0.5; burst = 3 }
  keys = @{
    $key = @{
      name      = 'public-trial'
      default   = $true
      qps       = 2.0
      burst     = 10.0
      ip_limits = @(
        @{ requests = 10;  per_seconds = 60   },
        @{ requests = 100; per_seconds = 3600 }
      )
    }
  }
}

$dir = Split-Path -Parent $Path
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

$json = $cfg | ConvertTo-Json -Depth 5
Set-Content -Path $Path -Value $json -Encoding utf8
Write-Host "Wrote $Path with a freshly-generated public-trial key."
