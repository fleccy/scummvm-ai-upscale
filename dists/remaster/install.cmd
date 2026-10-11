@echo off
setlocal
rem Started from a PowerShell 7 window, Windows PowerShell 5.1 would inherit PowerShell 7's module folders and
rem miss its own commands (Get-FileHash, Expand-Archive): let it use its own.
set "PSModulePath="
rem Double-click to install: opens the setup window (install-gui.ps1, which runs install.ps1 with your answers).
rem With arguments, or "install.cmd /console": the text version, install.ps1, as before.
rem Both run for this one process without changing your system's PowerShell policy.
if /i "%~1"=="/console" goto console
if not "%~1"=="" goto args
if not exist "%~dp0install-gui.ps1" goto console
echo Opening the setup window. This window closes by itself when setup is finished.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-gui.ps1"
if not "%errorlevel%"=="10" exit /b %errorlevel%
echo The setup window could not be opened; continuing in this window.
:console
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
goto end
:args
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
:end
echo.
pause
