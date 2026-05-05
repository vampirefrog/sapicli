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

## Install-Service.ps1 / Deploy-Remote.ps1

Coming in Step 3.
