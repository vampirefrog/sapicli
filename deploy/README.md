# deploy/

One-shot scripts for installing the Gitea Actions runner and (later) deploying
sapisrv to a remote Windows machine.

## runner-setup.ps1

Installs the [Gitea Actions runner](https://gitea.com/gitea/act_runner) as a
Windows service. Run from an **elevated PowerShell prompt**:

```powershell
.\deploy\runner-setup.ps1 -GiteaUrl https://git.vampi.tech -Token <token>
```

Get the registration token from your Gitea instance: **Site Administration →
Actions → Runners → Create new runner** (instance-wide), or from a repo's
**Settings → Actions → Runners** (repo-scoped).

The script also `winget install`s **CMake**, **Ninja**, and **Python 3.12** so
the build workflow can find them on `PATH`. Other prerequisites that must
already be present:

- **VS Build Tools 2022** with the **C++ ATL** workload, at the default
  install path
- **vcpkg** at `C:\vcpkg` with `vcpkg integrate install` already run
- **emsdk** at `C:\emsdk` with `emsdk activate latest` already run

The runner registers under labels `self-hosted,windows,x64`; the workflow
in `.gitea/workflows/build.yml` selects it via those labels.

Service name is `actrunner`. Reinstall with `-Force`.

## sapisrv.wxs (MSI installer source)

WiX v5 source for a per-machine MSI that:

- Copies `sapicli.exe`, `sapisrv.exe`, and `www\` to
  `C:\Program Files\sapisrv\`
- Registers `sapisrv` as a Windows Service (account: `NT AUTHORITY\NetworkService`,
  start type: auto, args: `run`)
- Reserves the URL ACL: `netsh http add urlacl url=http://+:8080/ user="NT AUTHORITY\NetworkService"`
- Drops a default `keys.json` at `C:\ProgramData\sapicli\keys.json` if not
  already present (preserved across upgrade and uninstall — your keys live)
- Starts the service immediately after install

On uninstall: stops + removes the service, removes the URL ACL, removes
program files. **`%ProgramData%\sapicli\` is intentionally left behind**
so logs and `keys.json` survive a reinstall.

### Build (local)

Prereqs: .NET SDK 8+, `wix` dotnet tool, `dist/` already staged with the
binaries + `www/` + `keys.default.json`.

```powershell
dotnet tool install --global wix --version "5.*"
wix build deploy\sapisrv.wxs `
  -d "DistDir=$pwd\dist" `
  -d "Version=0.1.0" `
  -arch x64 `
  -o dist\sapisrv-setup.msi
```

### Build (CI)

The `build` job in `.gitea/workflows/build.yml` does the above after
staging `dist/`. The MSI is uploaded as a separate `actions/upload-artifact`.
Version comes from the tag (`v0.1.2` → `0.1.2`) or `0.0.0` for branch
builds.

### Install / uninstall on a target machine

```powershell
msiexec /i sapisrv-setup.msi /quiet /norestart            # install
msiexec /x sapisrv-setup.msi /quiet /norestart            # uninstall
msiexec /i sapisrv-setup.msi /lv* install.log /quiet      # verbose log
```

After install, `sc query sapisrv` should show `RUNNING`. Logs land at
`C:\ProgramData\sapicli\logs\sapisrv-YYYYMMDD.log`.

## Deploy-Remote.ps1

Coming in a follow-up — will use PSRemoting to copy the MSI to the prod
VM and run `msiexec /i ... /quiet` against it.
