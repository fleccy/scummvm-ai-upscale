@echo off
setlocal
rem Started from a PowerShell 7 window, Windows PowerShell 5.1 would inherit PowerShell 7's module folders and
rem miss its own commands (Get-FileHash, Expand-Archive): let it use its own.
set "PSModulePath="
rem Widens the cutscenes from your own copy of the game with the AI Remaster Pipeline (see generate_cutscenes.ps1).
rem usage: generate_cutscenes.cmd -ArpDir <folder of the AI Remaster Pipeline>
if "%~1"=="" (
  set /p ARPDIR=Folder of your AI Remaster Pipeline install: 
) else (
  set ARPDIR=
)
if defined ARPDIR (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0generate_cutscenes.ps1" -ArpDir "%ARPDIR%"
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0generate_cutscenes.ps1" %*
)
echo.
pause
