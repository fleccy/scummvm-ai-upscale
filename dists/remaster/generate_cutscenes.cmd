@echo off
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
