@echo off
rem Generates widescreen side art from your own copy of the game (see generate_sides.ps1).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0generate_sides.ps1" %*
echo.
pause
