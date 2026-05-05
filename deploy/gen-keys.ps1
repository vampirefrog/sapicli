<#
.SYNOPSIS
  Generate %ProgramData%\sapicli\keys.json on first install if missing.

.DESCRIPTION
  Run as a custom action by sapisrv-setup.msi (and idempotent on its own).
  If -Path already exists, exits 0 without touching it -- the operator may
  have edited it. If it doesn't, writes a fresh keys.json containing:

    - public_tier  : safe defaults for unauthenticated requests (low qps)
    - one key      : 24 random bytes, base64url. Marked default=true so
                     the bundled web client picks it up via /api/default-key.
                     Rate limits: 10 requests/minute per IP and 100/hour per
                     IP, sized for a public-facing demo page.

  The key is *not* shown anywhere -- sysadmins can read it out of the
  emitted keys.json. On uninstall the file is preserved (Permanent="yes"
  on the parent directory in the WiX source).

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

# 24 random bytes -> 32 base64 chars (url-safe).
$bytes = New-Object byte[] 24
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$key = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')

$cfg = @{
  public_tier = @{ qps = 0.5; burst = 3 }
  keys = @{
    $key = @{
      name        = 'public-trial'
      default     = $true
      qps         = 2.0
      burst       = 10.0
      ip_limits   = @(
        @{ requests = 10;  per_seconds = 60   },
        @{ requests = 100; per_seconds = 3600 }
      )
    }
  }
}

# Make sure the parent dir exists (LogsDir component creates it on install,
# but be defensive for hand-runs).
$dir = Split-Path -Parent $Path
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

$json = $cfg | ConvertTo-Json -Depth 5
Set-Content -Path $Path -Value $json -Encoding utf8
Write-Host "Wrote $Path with a freshly-generated public-trial key."
