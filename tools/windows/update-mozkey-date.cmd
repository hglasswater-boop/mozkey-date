@echo off
setlocal EnableExtensions

set "BOOTSTRAP=%TEMP%\mozkey-date-updater-%RANDOM%-%RANDOM%.ps1"

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop';" ^
  "$headers = @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'mozkey-date-windows-bootstrap'; 'X-GitHub-Api-Version' = '2022-11-28' };" ^
  "$release = Invoke-RestMethod -Uri 'https://api.github.com/repos/hglasswater-boop/mozkey-date/releases/latest' -Headers $headers;" ^
  "$asset = $release.assets | Where-Object { $_.name -eq 'update-mozkey-date.ps1' } | Select-Object -First 1;" ^
  "if (-not $asset) { throw ('Release ' + $release.tag_name + ' に update-mozkey-date.ps1 がありません。') };" ^
  "Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -Headers $headers -OutFile '%BOOTSTRAP%'"

if errorlevel 1 (
  echo 最新の更新スクリプトを取得できませんでした。
  echo GitHub への接続を確認して、もう一度実行してください。
  if exist "%BOOTSTRAP%" del /q "%BOOTSTRAP%" >nul 2>&1
  pause
  exit /b 1
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%BOOTSTRAP%" %*
set "EXIT_CODE=%ERRORLEVEL%"

if exist "%BOOTSTRAP%" del /q "%BOOTSTRAP%" >nul 2>&1
if not "%EXIT_CODE%"=="0" pause
exit /b %EXIT_CODE%
