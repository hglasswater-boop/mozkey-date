[CmdletBinding()]
param(
  [switch]$Force,
  [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Repository = "hglasswater-boop/mozkey-date"
$InstallerAssetName = "MozkeyDate-Windows.msi"
$ChecksumAssetName = "$InstallerAssetName.sha256"
$StateDirectory = Join-Path $env:LOCALAPPDATA "MozkeyDate"
$InstalledReleaseFile = Join-Path $StateDirectory "last-installed-release.txt"
$LogDirectory = Join-Path $StateDirectory "Logs"
$ApiHeaders = @{
  Accept = "application/vnd.github+json"
  "User-Agent" = "mozkey-date-windows-updater"
  "X-GitHub-Api-Version" = "2022-11-28"
}

function Get-LatestRelease {
  $uri = "https://api.github.com/repos/$Repository/releases/latest"
  try {
    return Invoke-RestMethod -Uri $uri -Headers $ApiHeaders
  }
  catch {
    throw "最新版の取得に失敗しました。まだ GitHub Release が作成されていないか、GitHub に接続できません。`n$($_.Exception.Message)"
  }
}

function Get-ReleaseAsset([object]$Release, [string]$Name) {
  $asset = $Release.assets | Where-Object { $_.name -eq $Name } | Select-Object -First 1
  if (-not $asset) {
    throw "Release '$($Release.tag_name)' に $Name がありません。"
  }
  return $asset
}

function Get-InstallerErrorMessage([int]$ExitCode, [string]$LogPath) {
  switch ($ExitCode) {
    1602 { return "インストールがキャンセルされました。" }
    1603 { return "Windows Installer で致命的なエラーが発生しました。" }
    1618 { return "別の Windows Installer 処理が実行中です。完了後にもう一度更新してください。" }
    1619 { return "ダウンロードした MSI を開けませんでした。" }
    1638 { return "別バージョンの Mozkey が残っているため更新できませんでした。MSI の UpgradeCode / ProductVersion を確認してください。" }
    default { return "インストーラーが終了コード $ExitCode で失敗しました。" }
  }
}

$release = Get-LatestRelease
$tag = [string]$release.tag_name
if ([string]::IsNullOrWhiteSpace($tag)) {
  throw "Release のタグ名を取得できませんでした。"
}

$installedTag = $null
if (Test-Path $InstalledReleaseFile) {
  $installedTag = (Get-Content -LiteralPath $InstalledReleaseFile -Raw).Trim()
  if (-not $Force -and $installedTag -eq $tag) {
    Write-Host "Mozkey Date $tag はこの更新ツールですでにインストール済みです。"
    Write-Host "再インストールする場合は -Force を指定してください。"
    exit 0
  }
}

$installerAsset = Get-ReleaseAsset $release $InstallerAssetName
$checksumAsset = Get-ReleaseAsset $release $ChecksumAssetName
$tempDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("mozkey-date-update-" + [Guid]::NewGuid().ToString("N"))
$installerPath = Join-Path $tempDirectory $InstallerAssetName
$checksumPath = Join-Path $tempDirectory $ChecksumAssetName

New-Item -ItemType Directory -Path $tempDirectory -Force | Out-Null
try {
  Write-Host "Mozkey Date $tag をダウンロードしています..."
  Invoke-WebRequest -Uri $installerAsset.browser_download_url -Headers $ApiHeaders -OutFile $installerPath
  Invoke-WebRequest -Uri $checksumAsset.browser_download_url -Headers $ApiHeaders -OutFile $checksumPath

  $expectedHash = (((Get-Content -LiteralPath $checksumPath -Raw) -split '\s+')[0]).ToLowerInvariant()
  $actualHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($expectedHash -ne $actualHash) {
    throw "SHA-256 が一致しません。更新を中止しました。"
  }

  New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
  $safeTag = $tag -replace '[^0-9A-Za-z._-]', '_'
  $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $logPath = Join-Path $LogDirectory "update-$safeTag-$timestamp.log"

  Write-Host "SHA-256 を確認しました。インストーラーを起動します。"
  Write-Host "インストールログ: $logPath"
  $uiArgument = if ($Quiet) { "/qn" } else { "/passive" }
  $arguments = @(
    "/i",
    "`"$installerPath`"",
    $uiArgument,
    "/norestart",
    "/L*v",
    "`"$logPath`""
  )

  # A major upgrade has a different ProductCode and must be installed as a new
  # product. REINSTALL=ALL prevents installation when that ProductCode is not
  # already installed, so only use reinstall properties for an explicit
  # same-release -Force repair.
  if ($Force -and $installedTag -eq $tag) {
    $arguments += "REINSTALL=ALL"
    $arguments += "REINSTALLMODE=vomus"
  }

  try {
    $process = Start-Process -FilePath "msiexec.exe" -ArgumentList $arguments -Verb RunAs -Wait -PassThru
  }
  catch {
    throw "インストーラーを起動できませんでした。管理者権限の確認がキャンセルされたか、Windows Installer を起動できません。`n$($_.Exception.Message)"
  }

  if ($process.ExitCode -notin @(0, 1641, 3010)) {
    $message = Get-InstallerErrorMessage -ExitCode $process.ExitCode -LogPath $logPath
    throw "$message`n終了コード: $($process.ExitCode)`nログ: $logPath"
  }

  New-Item -ItemType Directory -Path $StateDirectory -Force | Out-Null
  # powershell.exe is Windows PowerShell 5.1 on stock Windows, where
  # utf8NoBOM is not a supported Set-Content encoding. The tag is ASCII.
  Set-Content -LiteralPath $InstalledReleaseFile -Value $tag -Encoding ASCII

  if ($process.ExitCode -in @(1641, 3010)) {
    Write-Host "Mozkey Date $tag への更新が完了しました。Windows の再起動が必要です。"
  }
  else {
    Write-Host "Mozkey Date $tag への更新が完了しました。"
  }
}
finally {
  Remove-Item -LiteralPath $tempDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
