[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$PreviousMsi,

  [Parameter(Mandatory = $true)]
  [string]$CandidateMsi,

  [string]$LogDirectory = (Join-Path $env:TEMP "mozkey-date-msi-upgrade-test")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-FullPath([string]$Path) {
  return [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
}

function Get-MsiProperty([string]$Path, [string]$PropertyName) {
  $installer = $null
  $database = $null
  $view = $null
  $record = $null
  try {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.GetType().InvokeMember(
      "OpenDatabase",
      [System.Reflection.BindingFlags]::InvokeMethod,
      $null,
      $installer,
      @($Path, 0)
    )
    $query = "SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = '$PropertyName'"
    $view = $database.GetType().InvokeMember(
      "OpenView",
      [System.Reflection.BindingFlags]::InvokeMethod,
      $null,
      $database,
      @($query)
    )
    $view.GetType().InvokeMember(
      "Execute",
      [System.Reflection.BindingFlags]::InvokeMethod,
      $null,
      $view,
      $null
    ) | Out-Null
    $record = $view.GetType().InvokeMember(
      "Fetch",
      [System.Reflection.BindingFlags]::InvokeMethod,
      $null,
      $view,
      $null
    )
    if ($null -eq $record) {
      throw "MSI property '$PropertyName' was not found in $Path"
    }

    $value = $record.GetType().InvokeMember(
      "StringData",
      [System.Reflection.BindingFlags]::GetProperty,
      $null,
      $record,
      @(1)
    )

    $view.GetType().InvokeMember(
      "Close",
      [System.Reflection.BindingFlags]::InvokeMethod,
      $null,
      $view,
      $null
    ) | Out-Null

    return [string]$value
  }
  finally {
    foreach ($comObject in @($record, $view, $database, $installer)) {
      if ($null -ne $comObject -and [System.Runtime.InteropServices.Marshal]::IsComObject($comObject)) {
        [System.Runtime.InteropServices.Marshal]::ReleaseComObject($comObject) | Out-Null
      }
    }
  }
}

function Invoke-MsiInstall([string]$Path, [string]$LogPath) {
  $arguments = @(
    "/i",
    "`"$Path`"",
    "/qn",
    "/norestart",
    "/L*v",
    "`"$LogPath`""
  )
  $process = Start-Process -FilePath "msiexec.exe" -ArgumentList $arguments -Wait -PassThru
  if ($process.ExitCode -notin @(0, 1641, 3010)) {
    if (Test-Path -LiteralPath $LogPath) {
      Write-Host "--- MSI log tail: $LogPath ---"
      Get-Content -LiteralPath $LogPath -Tail 200
    }
    throw "msiexec failed for '$Path' with exit code $($process.ExitCode)."
  }
}

function Invoke-MsiUninstall([string]$ProductCode) {
  $process = Start-Process -FilePath "msiexec.exe" -ArgumentList @(
    "/x",
    $ProductCode,
    "/qn",
    "/norestart"
  ) -Wait -PassThru
  if ($process.ExitCode -notin @(0, 1605, 1641, 3010)) {
    Write-Warning "Cleanup uninstall for $ProductCode returned $($process.ExitCode)."
  }
}

$PreviousMsi = Get-FullPath $PreviousMsi
$CandidateMsi = Get-FullPath $CandidateMsi
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$LogDirectory = [System.IO.Path]::GetFullPath($LogDirectory)

$previousProductCode = Get-MsiProperty $PreviousMsi "ProductCode"
$candidateProductCode = Get-MsiProperty $CandidateMsi "ProductCode"
$previousUpgradeCode = Get-MsiProperty $PreviousMsi "UpgradeCode"
$candidateUpgradeCode = Get-MsiProperty $CandidateMsi "UpgradeCode"
$previousVersionText = Get-MsiProperty $PreviousMsi "ProductVersion"
$candidateVersionText = Get-MsiProperty $CandidateMsi "ProductVersion"

$previousVersion = [version]$previousVersionText
$candidateVersion = [version]$candidateVersionText

if ($previousProductCode -eq $candidateProductCode) {
  throw "ProductCode must change for a major upgrade: $candidateProductCode"
}
if ($previousUpgradeCode -ne $candidateUpgradeCode) {
  throw "UpgradeCode changed: previous=$previousUpgradeCode candidate=$candidateUpgradeCode"
}
if ($candidateVersion -le $previousVersion) {
  throw "ProductVersion must increase: previous=$previousVersion candidate=$candidateVersion"
}

Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class MozkeyDateMsiNative {
  [DllImport("msi.dll", CharSet = CharSet.Unicode)]
  public static extern int MsiQueryProductState(string product);
}
'@

function Get-MsiProductState([string]$ProductCode) {
  return [MozkeyDateMsiNative]::MsiQueryProductState($ProductCode)
}

Write-Host "Previous MSI:  ProductVersion=$previousVersion ProductCode=$previousProductCode"
Write-Host "Candidate MSI: ProductVersion=$candidateVersion ProductCode=$candidateProductCode"
Write-Host "UpgradeCode:   $candidateUpgradeCode"

$previousLog = Join-Path $LogDirectory "previous-install.log"
$candidateLog = Join-Path $LogDirectory "candidate-upgrade.log"

try {
  Write-Host "Installing previous MSI..."
  Invoke-MsiInstall $PreviousMsi $previousLog
  if ((Get-MsiProductState $previousProductCode) -ne 5) {
    throw "Previous product was not registered as installed after installation."
  }

  Write-Host "Upgrading with candidate MSI..."
  Invoke-MsiInstall $CandidateMsi $candidateLog

  $candidateState = Get-MsiProductState $candidateProductCode
  $previousState = Get-MsiProductState $previousProductCode
  if ($candidateState -ne 5) {
    throw "Candidate product is not installed after the upgrade. State=$candidateState"
  }
  if ($previousState -eq 5) {
    throw "Previous product is still installed after the upgrade. Major upgrade replacement failed."
  }

  Write-Host "Major upgrade verified: old ProductCode was removed and candidate ProductCode is installed."
}
finally {
  if ((Get-MsiProductState $candidateProductCode) -eq 5) {
    Invoke-MsiUninstall $candidateProductCode
  }
  if ((Get-MsiProductState $previousProductCode) -eq 5) {
    Invoke-MsiUninstall $previousProductCode
  }
}
