param(
  [Parameter(Mandatory = $true)]
  [string]$CandidateMsi,
  [string]$ExpectedFileVersion = "100.2.2.0",
  [string]$ExpectedProductVersion = "v0.2.2",
  [string]$ExpectedMsiVersion = "100.2.2"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Invoke-Msi {
  param(
    [Parameter(Mandatory = $true)][string]$Arguments,
    [Parameter(Mandatory = $true)][string]$LogPath
  )

  $process = Start-Process -FilePath "msiexec.exe" `
    -ArgumentList "$Arguments /qn /norestart /L*v `"$LogPath`"" `
    -Wait -PassThru
  if ($process.ExitCode -notin @(0, 3010)) {
    if (Test-Path $LogPath) {
      Get-Content $LogPath -Tail 200 | Write-Host
    }
    throw "msiexec failed with exit code $($process.ExitCode)"
  }
}

function Get-MozkeyUninstallEntries {
  $roots = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
  )

  @(
    foreach ($root in $roots) {
      Get-ItemProperty $root -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -eq "Mozkey" }
    }
  )
}

$candidate = (Resolve-Path $CandidateMsi).Path
$tempDir = Join-Path $env:RUNNER_TEMP "mozkey-date-upgrade-smoke"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$oldMsi = Join-Path $tempDir "MozkeyDate-v0.2.0.msi"
$oldLog = Join-Path $tempDir "install-v0.2.0.log"
$upgradeLog = Join-Path $tempDir "upgrade-candidate.log"
$cleanupLog = Join-Path $tempDir "cleanup.log"
$toolPath = Join-Path $env:ProgramFiles "Mozc\mozc_tool.exe"

$oldUrl = "https://github.com/hglasswater-boop/mozkey-date/releases/download/v0.2.0/MozkeyDate-Windows.msi"
Write-Host "Downloading released v0.2.0 MSI..."
Invoke-WebRequest -Uri $oldUrl -OutFile $oldMsi

try {
  Write-Host "Installing released v0.2.0..."
  Invoke-Msi -Arguments "/i `"$oldMsi`"" -LogPath $oldLog

  if (-not (Test-Path $toolPath -PathType Leaf)) {
    throw "v0.2.0 did not install expected binary: $toolPath"
  }

  $oldInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($toolPath)
  $oldHash = (Get-FileHash $toolPath -Algorithm SHA256).Hash
  Write-Host "Old FileVersion=$($oldInfo.FileVersion) ProductVersion=$($oldInfo.ProductVersion) SHA256=$oldHash"

  Write-Host "Upgrading in place to candidate MSI..."
  Invoke-Msi -Arguments "/i `"$candidate`"" -LogPath $upgradeLog

  if (-not (Test-Path $toolPath -PathType Leaf)) {
    throw "Candidate upgrade left expected binary missing: $toolPath"
  }

  $newInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($toolPath)
  $newHash = (Get-FileHash $toolPath -Algorithm SHA256).Hash
  Write-Host "New FileVersion=$($newInfo.FileVersion) ProductVersion=$($newInfo.ProductVersion) SHA256=$newHash"

  if ($newHash -eq $oldHash) {
    throw "mozc_tool.exe was not replaced during upgrade; hash is unchanged."
  }
  if ($newInfo.FileVersion -ne $ExpectedFileVersion) {
    throw "Unexpected installed FileVersion: $($newInfo.FileVersion), expected $ExpectedFileVersion"
  }
  if ($newInfo.ProductVersion -ne $ExpectedProductVersion) {
    throw "Unexpected installed ProductVersion: $($newInfo.ProductVersion), expected $ExpectedProductVersion"
  }

  $entries = @(Get-MozkeyUninstallEntries)
  Write-Host "Mozkey uninstall entries: $($entries.Count)"
  foreach ($entry in $entries) {
    Write-Host "  DisplayVersion=$($entry.DisplayVersion) PSChildName=$($entry.PSChildName)"
  }
  if ($entries.Count -ne 1) {
    throw "Expected exactly one installed Mozkey product after upgrade, found $($entries.Count)."
  }
  if ($entries[0].DisplayVersion -ne $ExpectedMsiVersion) {
    throw "Unexpected installed MSI DisplayVersion: $($entries[0].DisplayVersion), expected $ExpectedMsiVersion"
  }

  Write-Host "Upgrade smoke test passed: released v0.2.0 was replaced by $ExpectedProductVersion."
}
finally {
  if (Test-Path $candidate) {
    try {
      Invoke-Msi -Arguments "/x `"$candidate`"" -LogPath $cleanupLog
    }
    catch {
      Write-Warning "Cleanup uninstall failed: $_"
    }
  }
}
