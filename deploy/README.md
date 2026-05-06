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

## Deploy-Remote.ps1 (coming soon)

PSRemoting from CI to push the zip to the prod VM, stop the service, swap
files, restart. Tag-push only.
