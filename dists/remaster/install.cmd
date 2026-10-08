@echo off
setlocal
rem Started from a PowerShell 7 window, Windows PowerShell 5.1 would inherit PowerShell 7's module folders and
rem miss its own commands (Get-FileHash, Expand-Archive): let it use its own.
set "PSModulePath="
rem Double-click to install. Runs install.ps1 for this one process without changing your system's PowerShell policy.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
