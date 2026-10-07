@echo off
rem Double-click to install. Runs install.ps1 for this one process without changing your system's PowerShell policy.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
