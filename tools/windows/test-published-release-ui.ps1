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

function Find-Element(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$AutomationIdSuffix,
    [string[]]$Names) {
  $elements = $Root.FindAll(
    [System.Windows.Automation.TreeScope]::Subtree,
    [System.Windows.Automation.Condition]::TrueCondition
  )
  for ($i = 0; $i -lt $elements.Count; ++$i) {
    $element = $elements.Item($i)
    try {
      $automationId = [string]$element.Current.AutomationId
      if (-not [string]::IsNullOrWhiteSpace($AutomationIdSuffix) -and
          ($automationId -eq $AutomationIdSuffix -or
           $automationId.EndsWith(".$AutomationIdSuffix"))) {
        return $element
      }
      $name = [string]$element.Current.Name
      foreach ($candidate in $Names) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            $name -eq $candidate) {
          return $element
        }
      }
    }
    catch {
    }
  }
  return $null
}

function Wait-ForWindow(
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
  throw "$Description did not create an accessible window."
}

function Write-UiTree(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Path) {
  $lines = New-Object System.Collections.Generic.List[string]
  $elements = $Root.FindAll(
    [System.Windows.Automation.TreeScope]::Subtree,
    [System.Windows.Automation.Condition]::TrueCondition
  )
  for ($i = 0; $i -lt $elements.Count; ++$i) {
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

function Save-Screenshot(
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

function Stop-Tool([System.Diagnostics.Process]$Process) {
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

function Start-ToolDialog(
    [string]$MozcTool,
    [string]$Mode,
    [string]$Description) {
  $process = Start-Process -FilePath $MozcTool -ArgumentList "--mode=$Mode" -PassThru
  try {
    return [pscustomobject]@{
      Process = $process
      Root = Wait-ForWindow $process.Id $Description
    }
  }
  catch {
    Stop-Tool $process
    throw
  }
}

function Select-Tab(
    [System.Windows.Automation.AutomationElement]$Root,
    [string]$Name) {
  $tab = Find-Element $Root "" @($Name)
  if ($null -eq $tab -or
      $tab.Current.ControlType -ne [System.Windows.Automation.ControlType]::TabItem) {
    throw "Config dialog does not expose the '$Name' tab."
  }
  $pattern = $tab.GetCurrentPattern(
    [System.Windows.Automation.SelectionItemPattern]::Pattern
  )
  ([System.Windows.Automation.SelectionItemPattern]$pattern).Select()
  Start-Sleep -Milliseconds 750
}

if ($ReleaseTag -notmatch '^v([0-9]+)\.([0-9]+)\.([0-9]+)$') {
  throw "ReleaseTag must be vX.Y.Z: $ReleaseTag"
}
$releaseMajor = [int]$Matches[1]
$releaseMinor = [int]$Matches[2]
$releasePatch = [int]$Matches[3]
$expectedFileVersion = "$(100 + $releaseMajor).$releaseMinor.$releasePatch.0"

$MsiPath = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $MsiPath).Path)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$installLog = Join-Path $OutputDirectory "install.log"

$installed = $false
$about = $null
$config = $null
try {
  $install = Start-Process -FilePath "msiexec.exe" -ArgumentList @(
    "/i", "`"$MsiPath`"", "/qn", "/norestart", "/L*v", "`"$installLog`""
  ) -Wait -PassThru
  if ($install.ExitCode -notin @(0, 1641, 3010)) {
    throw "Released MSI installation failed with exit code $($install.ExitCode)."
  }
  $installed = $true

  $candidatePaths = @(
    (Join-Path ${env:ProgramFiles(x86)} "Mozc\mozc_tool.exe"),
    (Join-Path $env:ProgramFiles "Mozc\mozc_tool.exe")
  ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  $mozcTool = $candidatePaths | Where-Object {
    Test-Path -LiteralPath $_ -PathType Leaf
  } | Select-Object -First 1
  if ([string]::IsNullOrWhiteSpace($mozcTool)) {
    throw "Installed mozc_tool.exe was not found under Program Files."
  }

  $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($mozcTool)
  if (([string]$versionInfo.FileVersion).Trim() -ne $expectedFileVersion) {
    throw "Installed mozc_tool.exe FileVersion mismatch: expected=$expectedFileVersion actual=$($versionInfo.FileVersion)"
  }
  if (([string]$versionInfo.ProductVersion).Trim() -ne $ReleaseTag) {
    throw "Installed mozc_tool.exe ProductVersion mismatch: expected=$ReleaseTag actual=$($versionInfo.ProductVersion)"
  }

  Write-Host "Opening About dialog from released MSI."
  $about = Start-ToolDialog $mozcTool "about_dialog" "About dialog"
  Start-Sleep -Milliseconds 500
  Write-UiTree $about.Root (Join-Path $OutputDirectory "about-uia.txt")
  $versionElement = Find-Element $about.Root "version_label" @()
  if ($null -eq $versionElement -or
      -not ([string]$versionElement.Current.Name).Contains($ReleaseTag)) {
    throw "About dialog does not display expected release tag '$ReleaseTag'."
  }
  Save-Screenshot $about.Root (Join-Path $OutputDirectory "about.png")
  Write-Host "About dialog displays $ReleaseTag."
  Stop-Tool $about.Process
  $about = $null

  Write-Host "Opening Config dialog from released MSI."
  $config = Start-ToolDialog $mozcTool "config_dialog" "Config dialog"
  Start-Sleep -Milliseconds 750

  Select-Tab $config.Root "日付"
  Write-UiTree $config.Root (Join-Path $OutputDirectory "config-date-uia.txt")

  $dateEnable = Find-Element $config.Root "dateConversionCheckBox" @("日付・時刻変換を有効にする")
  $dateFormatList = Find-Element $config.Root "dateConversionFormatListWidget" @()
  $dateFormatLabel = Find-Element $config.Root "dateConversionFormatLabel" @("優先する日付フォーマット（上ほど候補の優先度が高くなります）")
  if ($null -eq $dateEnable) {
    throw "Date tab does not expose the date/time conversion checkbox."
  }
  if ($null -eq $dateFormatList -or $null -eq $dateFormatLabel) {
    throw "Date tab does not expose the date-format customization controls."
  }

  Save-Screenshot $config.Root (Join-Path $OutputDirectory "config-date.png")
  Write-Host "Date tab exposes date/time conversion and date-format customization UI."
}
finally {
  if ($null -ne $about) {
    Stop-Tool $about.Process
  }
  if ($null -ne $config) {
    Stop-Tool $config.Process
  }
  if ($installed) {
    $products = Get-CimInstance Win32_Product | Where-Object {
      $_.Name -match 'Mozc|Mozkey'
    }
    foreach ($product in $products) {
      if (-not [string]::IsNullOrWhiteSpace([string]$product.IdentifyingNumber)) {
        Start-Process -FilePath "msiexec.exe" -ArgumentList @(
          "/x", $product.IdentifyingNumber, "/qn", "/norestart"
        ) -Wait | Out-Null
      }
    }
  }
}

Write-Host "Published release UI smoke test passed for $ReleaseTag."
