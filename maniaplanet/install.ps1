# Installs (or removes) TM Vibrant Shaders for ManiaPlanet: copies d3d11.dll next to ManiaPlanet.exe.
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Dll <path to d3d11.dll>] [-GameDir <folder>] [-Uninstall]
# Install.cmd / Uninstall.cmd run it with a double click. Without -GameDir it looks for ManiaPlanet
# in Program Files and in the Steam libraries. Under Program Files it asks for administrator rights.
param(
    [string]$Dll = (Join-Path $PSScriptRoot "d3d11.dll"),
    [string]$GameDir = "",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"

function Find-ManiaPlanet {
    $candidates = @("${env:ProgramFiles(x86)}\ManiaPlanet", "$env:ProgramFiles\ManiaPlanet")
    # Steam: its own folder and every library in libraryfolders.vdf.
    $steam = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libraries = @($steam)
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += $m.Groups[1].Value -replace '\\\\', '\'
            }
        }
        foreach ($lib in $libraries) { $candidates += Join-Path $lib "steamapps\common\ManiaPlanet" }
    }
    foreach ($dir in $candidates) {
        if ($dir -and (Test-Path (Join-Path $dir "ManiaPlanet.exe"))) { return $dir }
    }
    return $null
}

if (-not $GameDir) {
    $GameDir = Find-ManiaPlanet
    if (-not $GameDir) { throw "ManiaPlanet.exe not found (Program Files, Steam). Pass the game folder: install.ps1 -GameDir <folder>" }
}
$target = Join-Path $GameDir "d3d11.dll"
if (-not (Test-Path (Join-Path $GameDir "ManiaPlanet.exe"))) { throw "ManiaPlanet.exe not found in $GameDir (pass -GameDir)." }
if (Get-Process -Name ManiaPlanet, ManiaPlanet32 -ErrorAction SilentlyContinue) { throw "Close ManiaPlanet first." }

# A folder we can't write to (Program Files): once more with administrator rights.
$writable = $true
try {
    $probe = Join-Path $GameDir ".tmvs-write-test"
    [IO.File]::WriteAllText($probe, "")
    Remove-Item $probe -Force
} catch { $writable = $false }
if (-not $writable) {
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
Write-Host "Start ManiaPlanet (64-bit). F8 opens the menu, F5 switches the effects off and on, F12 captures a frame."
