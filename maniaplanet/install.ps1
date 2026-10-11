# Installs (or removes) TM Vibrant Shaders for ManiaPlanet: copies d3d11.dll next to ManiaPlanet.exe.
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Dll <path to d3d11.dll>] [-GameDir <folder>] [-Uninstall]
# The default game folder is under Program Files, so the script asks for administrator rights.
param(
    [string]$Dll = (Join-Path $PSScriptRoot "d3d11.dll"),
    [string]$GameDir = "${env:ProgramFiles(x86)}\ManiaPlanet",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"

$target = Join-Path $GameDir "d3d11.dll"
if (-not (Test-Path (Join-Path $GameDir "ManiaPlanet.exe"))) { throw "ManiaPlanet.exe not found in $GameDir (pass -GameDir)." }
if (Get-Process -Name ManiaPlanet, ManiaPlanet32 -ErrorAction SilentlyContinue) { throw "Close ManiaPlanet first." }

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Dll", "`"$Dll`"", "-GameDir", "`"$GameDir`"")
    if ($Uninstall) { $args += "-Uninstall" }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $args -Wait -PassThru
    exit $p.ExitCode
}

if ($Uninstall) {
    if (Test-Path $target) { Remove-Item $target -Force; Write-Host "Removed $target" } else { Write-Host "Nothing installed in $GameDir" }
    exit 0
}

if (-not (Test-Path $Dll)) { throw "d3d11.dll not found: $Dll" }
# Never overwrite a d3d11.dll that isn't ours (ReShade, another mod).
if (Test-Path $target) {
    $bytes = [IO.File]::ReadAllBytes($target)
    if (-not ([Text.Encoding]::ASCII.GetString($bytes).Contains("TmvsMpStats"))) {
        throw "$target exists and is not TM Vibrant Shaders (ReShade or another mod?). Remove it first."
    }
}
Copy-Item $Dll $target -Force
Write-Host "Installed $target"
Write-Host "Start ManiaPlanet (64-bit). F8 opens the menu, Shift+F8 switches the effects off and on."
