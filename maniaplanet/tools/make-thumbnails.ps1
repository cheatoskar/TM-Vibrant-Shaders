# Renders the preset pictures of the ManiaPlanet build's F8 menu (maniaplanet/thumbnails/*.jpg,
# embedded in d3d11.dll) from an F12 capture, with mp_preview. Run it again after changing a
# preset's look. TMNF has its own set (tools/make-thumbnails.ps1, thumbnails/).
#
#   powershell -File maniaplanet/tools/make-thumbnails.ps1 -Capture <capture folder> [-Preview build-mp/Release/mp_preview.exe]
#
# The pictures in the repo come from capture_034 of my 2026-10-17 session (Stadium by
# day, behind the car at the start, the sky above the start arch in view, 2560x1440): the day
# presets show the map as it is played; the black hole stands outside that view and the aurora
# is faint over it (capture_029, at night, showed those two better).
param(
    [Parameter(Mandatory = $true)][string]$Capture,
    [string]$Preview = ''
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults.
if (-not $Preview) { $Preview = "$PSScriptRoot\..\..\build-mp\Release\mp_preview.exe" }
$out = (Resolve-Path "$PSScriptRoot\..\thumbnails").Path
$capturePath = (Resolve-Path $Capture).Path
# mp_preview logs into TMVS_MP_DATA (default: Documents\ManiaPlanet\TMVS, the game's folder).
if (-not $env:TMVS_MP_DATA) { $env:TMVS_MP_DATA = Join-Path $env:TEMP 'tmvs-mp-thumbnails' }
New-Item -ItemType Directory -Force $env:TMVS_MP_DATA | Out-Null

# Same order as the Preset enum (src/config.h) and the ids in thumbnails.rc.
$presets = @('Vibrant', 'Realistic', 'Golden Hour', 'Dreamy', 'Neon', 'Horizon', 'Aurora', 'Competition',
             'Performance', 'Rainy', 'Replay Cinema', 'Storm', 'Snowstorm')
foreach ($p in $presets) {
    $file = ($p.ToLower() -replace ' ', '_') + '.jpg'
    # The HDR stage as in the game, 16 frames for TAA, exposure and the weather's particles.
    & $Preview $capturePath "$out\$file" --hdr --preset $p --frames 16 | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "mp_preview failed for $p" }
}
