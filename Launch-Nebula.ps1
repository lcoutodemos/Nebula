<#
.SYNOPSIS
Launches Nebula for Windows on this PC, building and redeploying it first only when needed.

.DESCRIPTION
Fast path: the package is registered from this build and nothing changed since, so the app
starts at once. Otherwise the native renderer is rebuilt when its sources are newer than its
DLL, the WinUI app is rebuilt, and winapp redeploys and launches the package. Build output
goes to artifacts\ in the repository root.

.PARAMETER Rebuild
Build and redeploy even if nothing looks stale.

.PARAMETER Check
Report whether a build is needed, without building or launching.
#>
[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$Check,
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'

$src = Join-Path $PSScriptRoot 'src'
$project = Join-Path $src 'Nebula.Windows\Nebula.Windows.csproj'
$manifest = [xml](Get-Content -LiteralPath (Join-Path $src 'Nebula.Windows\Package.appxmanifest') -Raw)
$identity = $manifest.Package.Identity.Name
$appId = @($manifest.Package.Applications.Application)[0].Id
$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'ARM64' } else { 'x64' }
$rid = if ($arch -eq 'ARM64') { 'win-arm64' } else { 'win-x64' }
$buildRoot = if ($env:NebulaBuildRoot) { $env:NebulaBuildRoot } else { Join-Path $PSScriptRoot 'artifacts' }
$nativeDll = Join-Path $buildRoot "Native\$arch\$Configuration\NebulaRender.dll"

function Get-Newest([string[]]$Folders) {
    $files = foreach ($folder in $Folders) {
        Get-ChildItem -LiteralPath (Join-Path $src $folder) -Recurse -File |
            Where-Object { $_.FullName -notmatch '\\(bin|obj|\.vs)\\' -and $_.Extension -ne '.md' }
    }
    ($files | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
}
function Get-Time([string]$Path) {
    if (Test-Path -LiteralPath $Path) { (Get-Item -LiteralPath $Path).LastWriteTimeUtc } else { [datetime]::MinValue }
}

$package = Get-AppxPackage -Name $identity -ErrorAction SilentlyContinue | Select-Object -First 1
$appx = Get-ChildItem -Path (Join-Path $buildRoot "Nebula.Windows\bin\$arch\$Configuration\*\$rid\AppX") -Directory -ErrorAction SilentlyContinue |
    Select-Object -First 1
$reasons = @()
if ($Rebuild) { $reasons += 'rebuild requested' }
if (-not $package) { $reasons += 'not installed' }
if (-not $appx) { $reasons += 'no deployed build' }
elseif ($package -and $package.InstallLocation -ne $appx.FullName) { $reasons += 'installed from a different build' }

$nativeStale = (Get-Time $nativeDll) -lt (Get-Newest @('Nebula.Render'))
if ($nativeStale) { $reasons += 'renderer sources changed' }
if ($appx) {
    $deployed = Get-Time (Join-Path $appx.FullName 'Nebula.Windows.dll')
    $rootInputs = @('Directory.Build.props', 'global.json', 'LICENSE', 'THIRD-PARTY-NOTICES.md') |
        ForEach-Object { Get-Time (Join-Path $PSScriptRoot $_) } | Sort-Object -Descending | Select-Object -First 1
    if ($deployed -lt (Get-Newest @('Nebula.Windows', 'Nebula.Core', 'Nebula.Embed')) -or $deployed -lt $rootInputs) { $reasons += 'app sources changed' }
    if ((Get-Time (Join-Path $appx.FullName 'NebulaRender.dll')) -lt (Get-Time $nativeDll)) { $reasons += 'renderer rebuilt' }
}

if ($Check) {
    if ($reasons.Count -eq 0) { 'Up to date: launches immediately.' } else { "Needs a build: $($reasons -join ', ')." }
    return
}
if ($reasons.Count -eq 0) {
    Start-Process "shell:AppsFolder\$($package.PackageFamilyName)!$appId"
    return
}

Write-Host "Nebula: building ($($reasons -join ', '))..." -ForegroundColor Cyan
if ($nativeStale -or $Rebuild) {
    Write-Host "  Native renderer ($arch $Configuration)"
    & (Join-Path $src 'Nebula.Render\build.ps1') -Configuration $Configuration -Architecture $arch
    if (-not (Test-Path -LiteralPath $nativeDll)) { throw "The renderer build did not produce $nativeDll." }
}
Write-Host '  WinUI app'
& dotnet build $project "-p:Platform=$arch" -c $Configuration -v:q -nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet build failed ($LASTEXITCODE)." }
if (-not (Get-Command winapp -ErrorAction SilentlyContinue)) {
    throw 'winapp (WinApp CLI 0.7+) is needed to deploy the package. Install it, then run this again.'
}
Write-Host '  Deploying and launching'
& winapp run $project --arch $arch.ToLowerInvariant() -c $Configuration --no-build --no-restore --detach --on local
if ($LASTEXITCODE -ne 0) { throw "winapp run failed ($LASTEXITCODE)." }
