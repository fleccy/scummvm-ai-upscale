@echo off
setlocal
rem Started from a PowerShell 7 window, Windows PowerShell 5.1 would inherit PowerShell 7's module folders and
rem miss its own commands (Get-FileHash, Expand-Archive): let it use its own.
set "PSModulePath="
rem Generates widescreen side art from your own copy of the game (see generate_sides.ps1).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0generate_sides.ps1" %*
echo.
pause
