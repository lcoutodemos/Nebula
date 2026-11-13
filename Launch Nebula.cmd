@echo off
rem Launches Nebula for Windows. Builds and redeploys first only when the sources changed.
rem Pass -Rebuild to force a build.
setlocal
set "SCRIPT=%~dp0Launch-Nebula.ps1"
where pwsh >nul 2>nul && (set "PS=pwsh") || (set "PS=powershell")
%PS% -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
if errorlevel 1 (
    echo.
    echo Nebula could not start. See the message above.
    pause
)
