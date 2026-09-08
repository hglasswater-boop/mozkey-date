@echo off
setlocal
rem Detach from this batch file before msiexec replaces the installed updater.
start "Mozkey Date Updater" powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0update-mozkey-date.ps1"
exit /b %ERRORLEVEL%
