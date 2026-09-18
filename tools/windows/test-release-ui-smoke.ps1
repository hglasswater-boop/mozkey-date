[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$ReleaseTag,

  [Parameter(Mandatory = $true)]
  [string]$MsiPath,

  [string]$OutputDirectory = (Join-Path $env:TEMP "mozkey-date-release-ui-smoke")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

function Get-FullPath([string]$Path) {
  return [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
}

function Get-MsiDatabaseValue(
    [string]$Path,
    [string]$Query,
    [string]$Description) {
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
    $view = $database.GetType().InvokeMember(
      "OpenView",
      [System.Reflection.BindingFlags]::InvokeMethod,
      $null,
      $database,
      @($Query)
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
      throw "$Description was not found in $Path"
    }
    return [string]$record.GetType().InvokeMember(
      "StringData",
      [System.Reflection.BindingFlags]::GetProperty,
      $null,
      $record,
      @(1)
    )
  }
  finally {
    if ($null -ne $view) {
      try {
        $view.GetType().InvokeMember(
          "Close",
          [System.Reflection.BindingFlags]::InvokeMethod,
          $null,
          $view,
          $null
        ) | Out-Null
      }
      catch {
      }
    }
    foreach ($comObject in @($record, $view, $database, $installer)) {
      if ($null -ne $comObject -and [System.Runtime.InteropServices.Marshal]::IsComObject($comObject)) {
        [System.Runtime.InteropServices.Marshal]::ReleaseComObject($comObject) | Out-Null
      }
    }
  }
}

function Get-MsiProperty([string]$Path, [string]$PropertyName) {
  $query = "SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = '$PropertyName'"
  return Get-MsiDatabaseValue $Path $query "MSI property '$PropertyName'"
}

function Get-MsiComponentId([string]$Path, [string]$ComponentName) {
  $query = "SELECT ``ComponentId`` FROM ``Component`` WHERE ``Component`` = '$ComponentName'"
  return Get-MsiDatabaseValue $Path $query "MSI component '$ComponentName'"
}

function Get-InstalledComponentPath(
    [string]$ProductCode,
    [string]$ComponentId,
    [string]$Description) {
  $installer = $null
  try {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $path = $installer.GetType().InvokeMember(
      "ComponentPath",
      [System.Reflection.BindingFlags]::GetProperty,
      $null,
      $installer,
      @($ProductCode, $ComponentId)
    )
    if ([string]::IsNullOrWhiteSpace([string]$path)) {
      throw "$Description does not have an installed key path."
    }
    $path = [System.IO.Path]::GetFullPath([string]$path)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
      throw "$Description was registered at '$path', but the file does not exist."
    }
    return $path
  }
  finally {
    if ($null -ne $installer -and [System.Runtime.InteropServices.Marshal]::IsComObject($installer)) {
      [System.Runtime.InteropServices.Marshal]::ReleaseComObject($installer) | Out-Null
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
      Get-Content -LiteralPath $LogPath -Tail 200
    }
    throw "msiexec failed with exit code $($process.ExitCode)."
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
    Write-Warning "Cleanup uninstall returned $($process.ExitCode)."
  }
}

function Wait-ForAutomationWindow(
    [int]$ProcessId,
    [string]$Description,
    [int]$TimeoutSeconds = 30) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
    $ProcessId
  )

  while ([DateTime]::UtcNow -lt $deadline) {
    $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
      [System.Windows.Automation.TreeScope]::Children,
      $condition
    )
    $best = $null
    $bestArea = 0.0
    foreach ($window in $windows) {
      try {
        $rect = $window.Current.BoundingRectangle
        $area = [double]$rect.Width * [double]$rect.Height
        if ($area -gt $bestArea) {
          $best = $window
          $bestArea = $area
        }
      }
      catch {
      }
    }
    if ($null -ne $best -and $bestArea -gt 0) {
      return $best
    }
    Start-Sleep -Milliseconds 250
  }

  throw "$Description did not create an accessible top-level window within $TimeoutSeconds seconds."
}

function Find-AutomationElement(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$AutomationId,
    [string[]]$NameContains) {
  $elements = $Root.FindAll(
    [System.Windows.Automation.TreeScope]::Subtree,
    [System.Windows.Automation.Condition]::TrueCondition
  )
  for ($i = 0; $i -lt $elements.Count; $i++) {
    $element = $elements.Item($i)
    try {
      if (-not [string]::IsNullOrWhiteSpace($AutomationId) -and
          $element.Current.AutomationId -eq $AutomationId) {
        return $element
      }
      $name = [string]$element.Current.Name
      foreach ($needle in $NameContains) {
        if (-not [string]::IsNullOrWhiteSpace($needle) -and $name.Contains($needle)) {
          return $element
        }
      }
    }
    catch {
    }
  }
  return $null
}

function Write-AutomationSnapshot(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Path) {
  $lines = New-Object System.Collections.Generic.List[string]
  $elements = $Root.FindAll(
    [System.Windows.Automation.TreeScope]::Subtree,
    [System.Windows.Automation.Condition]::TrueCondition
  )
  for ($i = 0; $i -lt $elements.Count; $i++) {
    $element = $elements.Item($i)
    try {
      $lines.Add(("Name='{0}' AutomationId='{1}' ControlType='{2}' Offscreen={3}" -f
        $element.Current.Name,
        $element.Current.AutomationId,
        $element.Current.ControlType.ProgrammaticName,
        $element.Current.IsOffscreen))
    }
    catch {
    }
  }
  $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Save-WindowScreenshot(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Path) {
  $rect = $Root.Current.BoundingRectangle
  $x = [int][Math]::Floor($rect.Left)
  $y = [int][Math]::Floor($rect.Top)
  $width = [int][Math]::Ceiling($rect.Width)
  $height = [int][Math]::Ceiling($rect.Height)
  if ($width -le 0 -or $height -le 0) {
    throw "Cannot capture a zero-sized window."
  }

  $bitmap = New-Object System.Drawing.Bitmap($width, $height)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  try {
    $graphics.CopyFromScreen($x, $y, 0, 0, $bitmap.Size)
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  }
  finally {
    $graphics.Dispose()
    $bitmap.Dispose()
  }
}

function Stop-DialogProcess([System.Diagnostics.Process]$Process) {
  if ($null -eq $Process) {
    return
  }
  try {
    $Process.Refresh()
    if (-not $Process.HasExited) {
      Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
      $Process.WaitForExit(5000) | Out-Null
    }
  }
  catch {
  }
}

function Start-Dialog(
    [string]$MozcTool,
    [string]$Mode,
    [string]$Description) {
  $process = Start-Process -FilePath $MozcTool -ArgumentList "--mode=$Mode" -PassThru
  try {
    $root = Wait-ForAutomationWindow $process.Id $Description
    return [pscustomobject]@{
      Process = $process
      Root = $root
    }
  }
  catch {
    Stop-DialogProcess $process
    throw
  }
}

if ($ReleaseTag -notmatch '^v([0-9]+)\.([0-9]+)\.([0-9]+)$') {
  throw "ReleaseTag must be vX.Y.Z: $ReleaseTag"
}
$releaseMajor = [int]$Matches[1]
$releaseMinor = [int]$Matches[2]
$releasePatch = [int]$Matches[3]
$expectedFileVersion = "$(100 + $releaseMajor).$releaseMinor.$releasePatch.0"
$expectedProductVersion = $ReleaseTag

$MsiPath = Get-FullPath $MsiPath
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$installLog = Join-Path $OutputDirectory "install.log"

$productCode = Get-MsiProperty $MsiPath "ProductCode"
$msiProductVersion = Get-MsiProperty $MsiPath "ProductVersion"
$mozcToolComponentId = Get-MsiComponentId $MsiPath "MozcTool"
$expectedMsiVersion = "$(100 + $releaseMajor).$releaseMinor.$releasePatch"
if ($msiProductVersion -ne $expectedMsiVersion) {
  throw "Released MSI ProductVersion mismatch: expected=$expectedMsiVersion actual=$msiProductVersion"
}

$about = $null
$config = $null
$installed = $false
try {
  Write-Host "Installing released MSI: $MsiPath"
  Invoke-MsiInstall $MsiPath $installLog
  $installed = $true

  $mozcTool = Get-InstalledComponentPath $productCode $mozcToolComponentId "mozc_tool.exe"
  $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($mozcTool)
  $actualFileVersion = ([string]$versionInfo.FileVersion).Trim()
  $actualProductVersion = ([string]$versionInfo.ProductVersion).Trim()
  if ($actualFileVersion -ne $expectedFileVersion) {
    throw "Installed mozc_tool.exe FileVersion mismatch: expected=$expectedFileVersion actual=$actualFileVersion"
  }
  if ($actualProductVersion -ne $expectedProductVersion) {
    throw "Installed mozc_tool.exe ProductVersion mismatch: expected=$expectedProductVersion actual=$actualProductVersion"
  }

  Write-Host "Opening About dialog from installed binary: $mozcTool"
  $about = Start-Dialog $mozcTool "about_dialog" "About dialog"
  Start-Sleep -Milliseconds 500
  Write-AutomationSnapshot $about.Root (Join-Path $OutputDirectory "about-uia.txt")
  $versionElement = Find-AutomationElement $about.Root "version_label" @($ReleaseTag)
  if ($null -eq $versionElement) {
    throw "About dialog does not expose expected release version '$ReleaseTag'."
  }
  Save-WindowScreenshot $about.Root (Join-Path $OutputDirectory "about.png")
  Write-Host "About dialog shows $ReleaseTag."
  Stop-DialogProcess $about.Process
  $about = $null

  Write-Host "Opening Config dialog from installed binary."
  $config = Start-Dialog $mozcTool "config_dialog" "Config dialog"
  Start-Sleep -Milliseconds 750

  $advancedTab = Find-AutomationElement $config.Root "inputsupport_tab" @("Advanced")
  if ($null -ne $advancedTab) {
    try {
      $selectionPattern = $advancedTab.GetCurrentPattern(
        [System.Windows.Automation.SelectionItemPattern]::Pattern
      )
      ([System.Windows.Automation.SelectionItemPattern]$selectionPattern).Select()
      Start-Sleep -Milliseconds 500
    }
    catch {
      Write-Warning "Could not select the Advanced tab through UI Automation: $_"
    }
  }

  Write-AutomationSnapshot $config.Root (Join-Path $OutputDirectory "config-uia.txt")
  $dateEnable = Find-AutomationElement $config.Root "dateConversionCheckBox" @("日付・時刻変換")
  $dateFormat = Find-AutomationElement $config.Root "dateConversionFormatListWidget" @("優先する日付フォーマット", "日付フォーマット")
  if ($null -eq $dateEnable) {
    throw "Config dialog does not expose the date/time conversion setting."
  }
  if ($null -eq $dateFormat) {
    throw "Config dialog does not expose the date-format customization UI."
  }

  try {
    $scrollPattern = $dateFormat.GetCurrentPattern(
      [System.Windows.Automation.ScrollItemPattern]::Pattern
    )
    ([System.Windows.Automation.ScrollItemPattern]$scrollPattern).ScrollIntoView()
    Start-Sleep -Milliseconds 500
  }
  catch {
    Write-Warning "Could not scroll the date-format control into view: $_"
  }

  Save-WindowScreenshot $config.Root (Join-Path $OutputDirectory "config-date.png")
  Write-Host "Config dialog exposes date/time conversion and date-format customization UI."
}
finally {
  if ($null -ne $about) {
    Stop-DialogProcess $about.Process
  }
  if ($null -ne $config) {
    Stop-DialogProcess $config.Process
  }
  if ($installed) {
    Invoke-MsiUninstall $productCode
  }
}

Write-Host "Release UI smoke test passed for $ReleaseTag."
