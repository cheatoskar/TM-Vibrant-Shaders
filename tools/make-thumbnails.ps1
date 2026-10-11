# Renders the preset pictures of the F8 menu (thumbnails/*.jpg, embedded in the DLL) from a
# frame capture, with the previewer. Run it again after changing a preset's look.
#
#   powershell -File tools/make-thumbnails.ps1 -Capture <capture.tmcap> [-Preview build/Release/tmvs_preview.exe]
#
# The pictures in the repo come from a capture at the Stadium start gate (car, sky and the sun
# in view), 1920x1080: every preset shows its sky, light and weather there.
param(
    [Parameter(Mandatory = $true)][string]$Capture,
    [string]$Preview = ''
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults.
if (-not $Preview) { $Preview = "$PSScriptRoot\..\build\Release\tmvs_preview.exe" }
$out = (Resolve-Path "$PSScriptRoot\..\thumbnails").Path
$capturePath = (Resolve-Path $Capture).Path

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
& $Preview $capturePath "$out\unused.bmp" --batch $jobs
Remove-Item $jobs
