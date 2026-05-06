# deploy/

Helper scripts for getting sapisrv onto a machine.

## runner-setup.ps1

Installs the [Gitea Actions runner](https://gitea.com/gitea/act_runner) as a
Windows service. Run from an **elevated PowerShell prompt**:

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy\runner-setup.ps1 `
  -GiteaUrl https://git.vampi.tech -Token <token>
```

Get the registration token from your Gitea instance: **Site Administration →
Actions → Runners → Create new runner** (instance-wide), or from a repo's
**Settings → Actions → Runners** (repo-scoped).

The script `winget install`s **CMake**, **Ninja**, **Python 3.12**, **Node
LTS**, **NSSM**, and downloads **PowerShell 7.5.3** as a machine-wide MSI so
the workflow's `shell: pwsh` steps work under the LocalSystem service. Other
prerequisites that must already be present:

- **VS Build Tools 2022** with the **C++ ATL** workload, at the default
  install path
- **vcpkg** at `C:\vcpkg`
- **emsdk** at `C:\emsdk` with `emsdk activate latest` already run

The runner registers under labels `self-hosted,windows,x64`; the workflow in
`.gitea/workflows/build.yml` selects it via those labels.

Service name is `actrunner` (NSSM-wrapped). Reinstall with `-Force`.

## Build artifact: sapisrv-<version>.zip

CI builds a zip from `dist/` containing:

```
sapicli.exe
sapisrv.exe
www/...
```

Per branch / PR build, version is `0.0.0`. On a tag push (`v0.1.2`), version
is `0.1.2`. Artifact name: `sapisrv-<version>-<sha>.zip`.

## Deploying by file copy (manual, today)

1. Download `sapisrv-<version>.zip` from the workflow run.
2. Copy to the target machine, unzip to `C:\Program Files\sapisrv\` (or
   wherever).
3. From an elevated prompt:
   ```powershell
   cd "C:\Program Files\sapisrv"
   netsh http add urlacl url=http://+:8080/ user="NT AUTHORITY\NetworkService"
   .\sapisrv.exe install
   sc start sapisrv
   ```
4. Drop a `keys.json` at `%ProgramData%\sapicli\keys.json` (any format the
   server understands -- see `server/auth.h` for the schema).

The server reads `SAPISRV_KEYS_JSON` env var if set, else falls back to
`%ProgramData%\sapicli\keys.json`. Logs land at
`%ProgramData%\sapicli\logs\sapisrv-YYYYMMDD.log`.

To pick a non-default port, pass `--port=N` to the service or to
`sapisrv.exe console`. The matching `netsh add urlacl` reservation has to
use the same port.

## Deploy-Remote.ps1

PSRemoting wrapper around the manual file-copy steps above. Run from any
machine with network reach + administrator credentials on the target:

```powershell
.\deploy\Deploy-Remote.ps1 `
  -ComputerName prod-vm `
  -ZipPath .\sapisrv-0.1.2.zip `
  -Port 8080
```

What it does on the remote:

1. Stops `sapisrv` service if running
2. Unzips into `C:\Program Files\sapisrv\` (overrides `-InstallDir`)
3. Re-creates the URL ACL (`netsh http add urlacl`) and the SCM service
   entry (`sc create`) with the current binPath + `--port=N` -- so an
   upgrade with a different port re-binds correctly
4. On a *first* deploy only, drops a `keys.json` with a randomly-generated
   public-trial key (rate limits: 10 req/min and 100 req/hour per IP).
   Operator-edited `keys.json` files are left alone forever.
5. Starts the service, waits for `RUNNING`, polls `/health` to confirm
   the HTTP listener is actually answering

Pass `-Credential (Get-Credential)` if the current user isn't already
admin on the target. PSRemoting must be enabled on the target
(`Enable-PSRemoting -Force` from an elevated prompt there, once).

The script is idempotent: re-running with a newer zip just swaps the
binaries and bounces the service. To deploy from CI on tag pushes,
add a job that does `actions/download-artifact` + invokes this script.
