<#
.SYNOPSIS
Builds Nebula: the native renderer, the WinUI app and the developer tools.

.DESCRIPTION
Restores NuGet packages, builds src\Nebula.Render (C++), src\Nebula.Windows (WinUI 3, MSIX) and the
tools under tools\. Build output goes to artifacts\ in the repository root.

.PARAMETER Architecture
ARM64 or x64. Defaults to this PC's architecture.

.PARAMETER Configuration
Debug (default) or Release.

.PARAMETER Test
Also run the renderer's GPU smoke tests and the core algorithm checks.

.PARAMETER Run
Deploy the package and launch the app (needs WinApp CLI and Developer Mode).

.EXAMPLE
.\build.ps1 -Test

.EXAMPLE
.\build.ps1 -Configuration Release -Run
#>
param(
    [ValidateSet('ARM64', 'x64')][string]$Architecture = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'ARM64' } else { 'x64' }),
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$Run,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'src\Nebula.Windows\Nebula.Windows.csproj'
$tools = @(
    (Join-Path $PSScriptRoot 'tools\Nebula.Embed.Cli\Nebula.Embed.Cli.csproj'),
    (Join-Path $PSScriptRoot 'tools\Nebula.Import\Nebula.Import.csproj')
)
$nativeBuild = Join-Path $PSScriptRoot 'src\Nebula.Render\build.ps1'

foreach ($tool in $tools) {
    & dotnet build $tool -c $Configuration --verbosity minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $tool ($LASTEXITCODE)." }
}

# The renderer build needs the WinUI interop header from the app's restored packages.
& dotnet restore $project "-p:Platform=$Architecture" "-p:Configuration=$Configuration" --verbosity minimal
if ($LASTEXITCODE -ne 0) { throw "App restore failed ($LASTEXITCODE)." }

& $nativeBuild -Architecture $Architecture -Configuration $Configuration -SmokeTest:$Test
if ($LASTEXITCODE -ne 0) { throw "Native renderer build failed ($LASTEXITCODE)." }

& dotnet build $project "-p:Platform=$Architecture" -c $Configuration --no-restore --verbosity minimal
if ($LASTEXITCODE -ne 0) { throw "WinUI build failed ($LASTEXITCODE)." }

if ($Test) {
    & dotnet run --project (Join-Path $PSScriptRoot 'tests\Nebula.Core.Tests\Nebula.Core.Tests.csproj') -c Release
    if ($LASTEXITCODE -ne 0) { throw "Core checks failed ($LASTEXITCODE)." }
}

if ($Run) {
    & winapp run $project --arch $Architecture.ToLowerInvariant() -c $Configuration --no-restore --detach --json
    if ($LASTEXITCODE -ne 0) { throw "Packaged app activation failed ($LASTEXITCODE)." }
}
