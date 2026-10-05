# Installs TM Vibrant Shaders into the TrackMania ModLoader.
#
# The ModLoader keeps a product database under %LOCALAPPDATA%\TMLoader,
# one folder per mod and one folder per version inside it, each with a description.yaml.
# This script sets up that layout so the loader immediately picks up the mod on its next start.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File install-modloader.ps1
#
# Parameters:
#   -Dll      Path to TMVibrantShaders.dll (defaults to build/Release or script directory)
#   -Version  Version string (default: "1.0.0")
#   -Products Custom products output folder (used for packaging zips in CI)

param(
  [string]$Dll = "",
  [string]$Version = "1.0.0",
  [string]$Products = ""
)

$ErrorActionPreference = "Stop"

# Auto-locate DLL if not explicitly passed
if (-not $Dll) {
  $candidates = @(
    "$PSScriptRoot\build\Release\TMVibrantShaders.dll",
    "$PSScriptRoot\build\Debug\TMVibrantShaders.dll",
    "$PSScriptRoot\TMVibrantShaders.dll",
    "$PSScriptRoot\1.0.0\TMVibrantShaders.dll"
  )
  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      $Dll = $candidate
      break
    }
  }
}

if (-not $Dll -or -not (Test-Path $Dll)) {
  Write-Warning "TMVibrantShaders.dll was not found yet. Shaders and Presets will be installed; please compile the project or place TMVibrantShaders.dll next to this script."
}

$loader = "$env:LOCALAPPDATA\TMLoader"
$packaging = $Products -ne ""
if (-not $packaging -and -not (Test-Path $loader)) {
  Write-Error "The TrackMania ModLoader does not seem to be installed ($loader is missing). Get it from https://tomashu.dev/software/tmloader/"
}

$products = if ($packaging) { $Products } else { "$loader\database\TmForever\products" }
$product  = "$products\TM Vibrant Shaders"
$target   = "$product\$Version"

New-Item -ItemType Directory -Force -Path $target | Out-Null
New-Item -ItemType Directory -Force -Path "$target\Shaders" | Out-Null
New-Item -ItemType Directory -Force -Path "$target\Presets" | Out-Null

$utf8 = New-Object System.Text.UTF8Encoding $false

# Root product metadata
[System.IO.File]::WriteAllText("$product\description.yaml", @"
name: TM Vibrant Shaders
author: cheatoskar
type: modification
homepage: 'https://github.com/cheatoskar/TM-Vibrant-Shaders'
description: 'Sildurs Vibrant, BSL & IterationT Minecraft-style shaders for TrackMania Nations & United Forever. Features volumetric sun rays, ACES filmic tonemapping, smart vibrance, bloom, and IterationT anamorphic lens flare streaks.'
"@, $utf8)

# Version metadata
[System.IO.File]::WriteAllText("$target\description.yaml", @"
executable: TMVibrantShaders.dll
dependencies:
  - id: CoreMod
    version: ^1.0.1
changelog: '- Initial release of Sildurs Vibrant, BSL, and IterationT shader suite with in-game F8 menu.'
"@, $utf8)

# Copy shaders and presets
if (Test-Path "$PSScriptRoot\Shaders") {
  Copy-Item -Path "$PSScriptRoot\Shaders\*" -Destination "$target\Shaders" -Recurse -Force
}
if (Test-Path "$PSScriptRoot\Presets") {
  Copy-Item -Path "$PSScriptRoot\Presets\*" -Destination "$target\Presets" -Recurse -Force
}

# Copy DLL if available
if ($Dll -and (Test-Path $Dll)) {
  Copy-Item $Dll "$target\TMVibrantShaders.dll" -Force
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Successfully installed TM Vibrant Shaders $Version to TMModloader!" -ForegroundColor Green
Write-Host "  Location: $target"
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Open the TrackMania ModLoader (TMLoader)."
Write-Host "  2. Tick 'TM Vibrant Shaders' in your active profile list."
Write-Host "  3. Start TrackMania Nations / United Forever."
Write-Host "  4. In game: Press [F8] to open the shader menu, [F7] to toggle on/off."
Write-Host ""
