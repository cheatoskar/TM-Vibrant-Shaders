# Renders the preset pictures of the F8 menu (thumbnails/*.jpg, embedded in the DLL) from a
# frame capture, with the previewer. Run it again after changing a preset's look.
#
#   powershell -File tools/make-thumbnails.ps1 -Capture <capture.tmcap> [-Preview build/Release/tmvs_preview.exe]
#
# The pictures in the repo come from a capture at the Stadium start gate (A04-Acrobatic, day, car,
# sky and the sun behind the arch in view; capture_109, 2026-10-17): every preset shows its sky,
# light and weather there, the night presets their night sky over the day map.
param(
    [Parameter(Mandatory = $true)][string]$Capture,
    [string]$Preview = ''
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults.
if (-not $Preview) { $Preview = "$PSScriptRoot\..\build\Release\tmvs_preview.exe" }
$Preview = (Resolve-Path $Preview).Path
$capturePath = (Resolve-Path $Capture).Path
# The batch file splits a job at spaces, and the repo often sits in a folder with one
# ("tm shaders"): the pictures are named relative to the repo instead.
Push-Location "$PSScriptRoot\.."
$out = 'thumbnails'

# Same order as the Preset enum (src/config.h) and the ids in thumbnails.rc.
$presets = @('Vibrant', 'Realistic', 'Golden Hour', 'Dreamy', 'Neon', 'Horizon', 'Aurora', 'Competition',
             'Performance', 'Rainy', 'Replay Cinema', 'Storm', 'Snowstorm')
$jobs = Join-Path $env:TEMP 'tmvs_thumbnails.txt'
$lines = foreach ($p in $presets) {
    $file = ($p.ToLower() -replace ' ', '_') + '.jpg'
    # Preset names with spaces are written with underscores in batch files.
    "$out\$file cap=$capturePath preset=$($p -replace ' ', '_')"
}
Set-Content -Path $jobs -Value $lines -Encoding ascii
try {
    & $Preview $capturePath "$out\unused.bmp" --batch $jobs
} finally {
    Pop-Location
    Remove-Item $jobs
}
