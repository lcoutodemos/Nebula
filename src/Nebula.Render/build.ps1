[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('ARM64', 'x64')][string]$Architecture = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'ARM64' } else { 'x64' }),
    [string]$OutputDirectory,
    [string]$WinUIIncludeDirectory,
    [switch]$SmokeTest,
    [switch]$BuildTestsOnly,
    [switch]$StressOnly,
    [string]$LibraryPath,
    [switch]$RealLibraryOnly,
    [switch]$DecodeAuditOnly,
    [switch]$FootprintOnly
)
$ErrorActionPreference = 'Stop'
if ($RealLibraryOnly -and !$LibraryPath) { throw '-RealLibraryOnly requires -LibraryPath.' }
if ($DecodeAuditOnly -and !$LibraryPath) { throw '-DecodeAuditOnly requires -LibraryPath.' }
if ($StressOnly -and ($RealLibraryOnly -or $LibraryPath)) { throw '-StressOnly cannot be combined with real-library tests.' }
if ($LibraryPath) { $LibraryPath = (Resolve-Path -LiteralPath $LibraryPath).Path }
if (!$OutputDirectory) {
    $buildRoot = if ($env:NebulaBuildRoot) { $env:NebulaBuildRoot } else { Join-Path $PSScriptRoot '..\..\artifacts' }
    $OutputDirectory = Join-Path $buildRoot "Native\$Architecture\$Configuration"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
foreach ($root in @($env:OneDrive, $env:OneDriveCommercial, $env:OneDriveConsumer)) {
    if ($root -and ($OutputDirectory.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $OutputDirectory.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase))) {
        Write-Warning 'Build output is inside a OneDrive folder and will sync. Clone the repository outside OneDrive, or set NebulaBuildRoot.'
        break
    }
}
if (!$WinUIIncludeDirectory) {
    # Use the WinUI package version the app resolved, so the header matches the shipped runtime.
    $packageRoot = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget\packages' }
    $lockFile = Join-Path $PSScriptRoot '..\Nebula.Windows\packages.lock.json'
    $winuiVersion = if (Test-Path -LiteralPath $lockFile) {
        (Get-Content -LiteralPath $lockFile -Raw | ConvertFrom-Json).dependencies.PSObject.Properties.Value |
            ForEach-Object { $_.'Microsoft.WindowsAppSDK.WinUI'.resolved } | Where-Object { $_ } | Select-Object -First 1
    }
    $winuiRoot = Join-Path $packageRoot 'microsoft.windowsappsdk.winui'
    if ($winuiVersion) { $winuiRoot = Join-Path $winuiRoot $winuiVersion }
    $headers = @(Get-ChildItem $winuiRoot -Filter 'microsoft.ui.xaml.media.dxinterop.h' -Recurse -ErrorAction SilentlyContinue)
    if (!$headers) { throw "Restore the WinUI app first (dotnet restore src\Nebula.Windows), or pass -WinUIIncludeDirectory." }
    $WinUIIncludeDirectory = ($headers | Sort-Object FullName -Descending | Select-Object -First 1).DirectoryName
}
$header = Join-Path $WinUIIncludeDirectory 'microsoft.ui.xaml.media.dxinterop.h'
if (!(Test-Path $header)) { throw "Missing WinUI 3 interop header: $header" }
if (!(Select-String -Path $header -Pattern '63aad0b8-7c24-40ff-85a8-640d944cc325' -Quiet)) {
    throw 'The supplied header is not the verified WinUI 3 ISwapChainPanelNative contract.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installations = @()
if (Test-Path $vswhere) {
    $installations += & $vswhere -all -prerelease -products '*' -property installationPath
}
$hostIsArm64 = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64'
# The renderer uses the target toolchain; the shader tool runs during the build, so it uses the host's.
# An Arm64 PC can also run x64 tools under emulation, so those are the fallback there.
$environments = switch ("$Architecture|$hostIsArm64") {
    'ARM64|True' { @('vcvarsarm64.bat', 'vcvarsamd64_arm64.bat') }
    'ARM64|False' { @('vcvarsamd64_arm64.bat') }
    'x64|True' { @('vcvarsarm64_amd64.bat', 'vcvars64.bat') }
    default { @('vcvars64.bat') }
}
$hostEnvironments = if ($hostIsArm64) { @('vcvarsarm64.bat', 'vcvars64.bat') } else { @('vcvars64.bat') }
function Find-VcVars([string[]]$Names) {
    $installations | ForEach-Object {
        $installation = $_
        $Names | ForEach-Object { Join-Path $installation "VC\Auxiliary\Build\$_" }
    } | Where-Object { Test-Path $_ } | Select-Object -First 1
}
$vcvars = Find-VcVars $environments
if (!$vcvars) { throw "Visual Studio C++ $Architecture toolchain not found. Install the MSVC build tools for $Architecture." }
$hostVcvars = Find-VcVars $hostEnvironments
if (!$hostVcvars) { throw 'Visual Studio C++ toolchain for this PC not found.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$optimization = if ($Configuration -eq 'Debug') { '/Od /Zi /D_DEBUG' } else { '/O2 /Zi /DNDEBUG' }
$runtime = if ($Configuration -eq 'Debug') { '/MTd' } else { '/MT' }
$common = "/nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /guard:cf $runtime /DUNICODE /D_UNICODE $optimization /I`"$WinUIIncludeDirectory`" /I`"$PSScriptRoot`" /I`"$OutputDirectory`""
$source = Join-Path $PSScriptRoot 'NebulaRender.cpp'
$dll = Join-Path $OutputDirectory 'NebulaRender.dll'
$obj = Join-Path $OutputDirectory 'NebulaRender.obj'
$pdb = Join-Path $OutputDirectory 'NebulaRender.pdb'
$compilerPdb = Join-Path $OutputDirectory 'NebulaRender.compiler.pdb'
$lib = Join-Path $OutputDirectory 'NebulaRender.lib'
$setup = "set `"PATH=$(Split-Path $vswhere);%PATH%`" && call `"$vcvars`" >nul"
$hostSetup = "set `"PATH=$(Split-Path $vswhere);%PATH%`" && call `"$hostVcvars`" >nul"
$shaderSource = Join-Path $PSScriptRoot 'CompileShaders.cpp'
$shaderExe = Join-Path $OutputDirectory 'CompileShaders.exe'
$shaderObj = Join-Path $OutputDirectory 'CompileShaders.obj'
$shaderPdb = Join-Path $OutputDirectory 'CompileShaders.compiler.pdb'
$shaderLinkPdb = Join-Path $OutputDirectory 'CompileShaders.pdb'
$shaderHeader = Join-Path $OutputDirectory 'CompiledShaders.h'
$shaderCommand = "$hostSetup && cl $common `"$shaderSource`" /Fo`"$shaderObj`" /Fd`"$shaderPdb`" /link /OUT:`"$shaderExe`" /PDB:`"$shaderLinkPdb`" /INCREMENTAL:NO d3dcompiler.lib"
& $env:ComSpec /d /s /c $shaderCommand
if ($LASTEXITCODE) { throw "Shader build tool failed ($LASTEXITCODE)." }
& $shaderExe $shaderHeader
if ($LASTEXITCODE) { throw "Shader precompilation failed ($LASTEXITCODE)." }
$encoder = Join-Path $PSScriptRoot 'vendor\bc7enc\bc7enc.cpp'
$encoderObj = Join-Path $OutputDirectory 'bc7enc.obj'
$encoderPdb = Join-Path $OutputDirectory 'bc7enc.pdb'
$encoderFlags = $common.Replace('/W4', '/W0').Replace('/Od', '/O2')
$command = "$setup && cl $encoderFlags /c `"$encoder`" /Fo`"$encoderObj`" /Fd`"$encoderPdb`" && cl $common /LD `"$source`" /Fo`"$obj`" /Fd`"$compilerPdb`" /link /guard:cf /OUT:`"$dll`" /PDB:`"$pdb`" /IMPLIB:`"$lib`" /INCREMENTAL:NO `"$encoderObj`" d3d11.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib shell32.lib bcrypt.lib"
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE) { throw "Native renderer build failed ($LASTEXITCODE)." }
Write-Host "Built $dll"
if ($SmokeTest -or $BuildTestsOnly -or $StressOnly -or $LibraryPath -or $FootprintOnly) {
    $testSource = Join-Path $PSScriptRoot 'smoke.cpp'
    $testExe = Join-Path $OutputDirectory 'NebulaRenderSmoke.exe'
    $testObj = Join-Path $OutputDirectory 'NebulaRenderSmoke.obj'
    $testPdb = Join-Path $OutputDirectory 'NebulaRenderSmoke.pdb'
    $testCompilerPdb = Join-Path $OutputDirectory 'NebulaRenderSmoke.compiler.pdb'
    $testCommand = "$setup && cl $common /guard:cf `"$testSource`" /Fo`"$testObj`" /Fd`"$testCompilerPdb`" /link /guard:cf /OUT:`"$testExe`" /PDB:`"$testPdb`" /INCREMENTAL:NO `"$lib`" `"$encoderObj`" ole32.lib runtimeobject.lib dxgi.lib d3d11.lib d3dcompiler.lib windowscodecs.lib shell32.lib bcrypt.lib"
    & $env:ComSpec /d /s /c $testCommand
    if ($LASTEXITCODE) { throw "Smoke test build failed ($LASTEXITCODE)." }
    if ($BuildTestsOnly) { Write-Host "Built $testExe (not run)"; return }
    if ($FootprintOnly) { & $testExe $OutputDirectory '--footprint-only' }
    elseif ($StressOnly) { & $testExe $OutputDirectory '--stress-only' }
    elseif ($DecodeAuditOnly) { & $testExe $OutputDirectory '--decode-audit-only' $LibraryPath }
    elseif ($RealLibraryOnly) { & $testExe $OutputDirectory '--real-library-only' $LibraryPath }
    elseif ($LibraryPath) { & $testExe $OutputDirectory '--library' $LibraryPath }
    else { & $testExe $OutputDirectory }
    if ($LASTEXITCODE) { throw "Smoke test failed ($LASTEXITCODE)." }
}
