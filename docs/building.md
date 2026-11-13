# Building and running

## Prerequisites

| Tool | Why |
|---|---|
| Windows 11, x64 or Arm64 | Nebula builds natively on both. Arm64 PCs can also cross-build x64. |
| [.NET 10 SDK](https://dotnet.microsoft.com/download) | The app, libraries and tools. `global.json` accepts any 10.0 SDK feature band. |
| Visual Studio 2026 (or 2022) with **WinUI application development** and **Desktop development with C++** | The MSVC toolset and Windows SDK for the renderer, and the MSIX tooling. Install the MSVC build tools for each architecture you build (ARM64, x64). |
| [WinApp CLI](https://github.com/microsoft/winappCli) 0.7 or later | Registers and launches the packaged app from the command line (`build.ps1 -Run`, `Launch Nebula.cmd`). |
| [Developer Mode](https://learn.microsoft.com/windows/apps/get-started/enable-your-device-for-development) | Lets Windows register a development MSIX package. |
| Python 3.12 (optional) | Only the tools use it: `tools/prepare_models.py` (model pack), `tools/make_icons.py` and `tests/create_fixtures.py` (both need Pillow). The app does not. |

NuGet restores everything else, including the Windows App SDK. Versions are pinned in the project files and
`packages.lock.json`.

## Build from the command line

```powershell
.\build.ps1                       # Debug build for this PC's architecture
.\build.ps1 -Test                 # also run the GPU smoke tests and the core checks
.\build.ps1 -Configuration Release -Run   # Release build, deploy and launch
.\build.ps1 -Architecture x64     # cross-build x64 on an Arm64 PC
```

`build.ps1` builds the tools, restores the app, builds the C++ renderer with `src\Nebula.Render\build.ps1`,
then builds the WinUI app. `-Run` deploys the package with `winapp run` and launches it.

## Build in Visual Studio

Open `Nebula.slnx`, set the solution platform to **ARM64** or **x64**, make `Nebula.Windows` the startup
project and press F5. The app project builds the renderer before it compiles. The renderer is rebuilt only
when a file under `src\Nebula.Render` is newer than its DLL. Set the MSBuild property
`NebulaSkipNativeBuild=true` to use a renderer you built yourself.

The C++ sources appear under `src\Nebula.Render` in Solution Explorer. They are built by
`src\Nebula.Render\build.ps1`, which calls the MSVC compiler directly, rather than by a `.vcxproj`.

## Launch without Visual Studio

Double-click `Launch Nebula.cmd` in the repository root. If the installed package is current, the app
starts at once. If the package is missing or any source is newer than the deployed build, it rebuilds
(Release, this PC's architecture) and redeploys first.

```powershell
.\Launch-Nebula.ps1 -Check      # report whether a build is needed
.\Launch-Nebula.ps1 -Rebuild    # force a rebuild and redeploy
```

The app is MSIX-packaged and needs package identity. Don't run `Nebula.Windows.exe` from the build folder.

## Where build output goes

`Directory.Build.props` sends all intermediate and output files to `artifacts\` in the repository root
(ignored by git):

| Path | Contents |
|---|---|
| `artifacts\<project>\obj`, `bin` | .NET build output, including the AppX layout the package is registered from |
| `artifacts\Native\<ARM64\|x64>\<Debug\|Release>` | `NebulaRender.dll`, its PDB, precompiled shaders and the smoke test |

Set the `NebulaBuildRoot` environment variable (or MSBuild property) to put the output somewhere else. Keep
it outside `%LOCALAPPDATA%`: WinApp CLI can't register a package from a folder under AppData. Clone the
repository outside OneDrive too, or the build output will sync; the renderer build warns if it does.

## Package identity and signing

`src\Nebula.Windows\Package.appxmanifest` carries a development identity (`CN=AppPublisher`). Visual Studio
and WinApp CLI sign development builds with a local test certificate. To distribute Nebula, set your own
publisher in the manifest and sign the package with your certificate. Never commit certificates or keys.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Restore the WinUI app first` from the renderer build | Run `dotnet restore src\Nebula.Windows -p:Platform=ARM64` (or x64), or use `build.ps1`, which restores first. |
| `Visual Studio C++ <arch> toolchain not found` | Install the MSVC build tools for that architecture in the Visual Studio Installer. |
| `winapp` is not recognized | Install WinApp CLI and open a new terminal. |
| Deployment fails with a developer license error | Turn on Developer Mode. |
| Search says it matches metadata only, and Similarity is unavailable | The AI model pack is not installed. See [local-ai.md](local-ai.md). |
| Anything else | Check `application.log` in the app's data folder (see [architecture.md](architecture.md#local-data)). |
