# Installs a local build of TM Vibrant Shaders into the TrackMania ModLoader.
# (Players use TM-Vibrant-Shaders-Setup.exe from the release zip instead.)
#
# The ModLoader keeps one folder per mod and per version, each with a description.yaml:
#   %LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Vibrant Shaders\<version>\
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File install-modloader.ps1 [-Dll <path>] [-Version 1.0.0]

param(
  [string]$Dll = "$PSScriptRoot\build\Release\TMVibrantShaders.dll",
  [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Dll)) {
  Write-Error "$Dll not found. Build first: cmake -B build -A Win32; cmake --build build --config Release"
}
if (Get-Process -Name TmForever -ErrorAction SilentlyContinue) {
  Write-Error "TrackMania is running. Close the game first."
}

$loader = "$env:LOCALAPPDATA\TMLoader"
if (-not (Test-Path $loader)) {
  Write-Error "The TrackMania ModLoader is not installed ($loader is missing). Get it from https://tomashu.dev/software/tmloader/"
}

$product = "$loader\database\TmForever\products\TM Vibrant Shaders"
$target = "$product\$Version"
if (Test-Path $product) { Remove-Item -Recurse -Force $product }
New-Item -ItemType Directory -Force -Path $target | Out-Null

Copy-Item "$PSScriptRoot\packaging\description.yaml" "$product\description.yaml"
Copy-Item "$PSScriptRoot\packaging\version.yaml" "$target\description.yaml"
Copy-Item $Dll "$target\TMVibrantShaders.dll"

Write-Host "Installed TM Vibrant Shaders $Version to $target" -ForegroundColor Green
Write-Host "Open the ModLoader, tick 'TM Vibrant Shaders' and start the game. In game: F8 = menu, F7 = on/off."
