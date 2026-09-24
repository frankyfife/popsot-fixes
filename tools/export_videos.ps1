# Exports the game's Bink videos (Video\*.int) as lossless-quality ProRes files
# for upscaling (e.g. Topaz Video AI). Only the picture is exported: the game
# keeps playing the original Bink file for sound, timing and language tracks.
#
# Usage:  powershell -ExecutionPolicy Bypass -File export_videos.ps1 `
#             -GameDir "F:\GOG Games\Prince of Persia - The Sands of Time" `
#             -OutDir "D:\pop_videos" [-FFmpeg "C:\path\to\ffmpeg.exe"]
#
# After upscaling, save each result as <name>.mp4 (H.264, or H.265/AV1 if the
# Windows codec extensions are installed) next to the original in the game's
# Video folder, e.g. Video\cine_010.mp4 for Video\cine_010.int. The length must
# stay the same; size (up to 4096x4096) and frame rate may change.
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$FFmpeg = "ffmpeg"
)

$videoDir = Join-Path $GameDir "Video"
if (-not (Test-Path $videoDir)) { throw "No Video folder in $GameDir" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

foreach ($file in Get-ChildItem $videoDir -Filter *.int) {
    $out = Join-Path $OutDir ($file.BaseName + ".mov")
    if (Test-Path $out) { Write-Host "skip  $($file.Name) (exists)"; continue }
    Write-Host "export $($file.Name)"
    # ProRes 422 HQ, 10 bit 4:2:2: visually lossless and read by every upscaler.
    & $FFmpeg -hide_banner -loglevel error -f bink -i $file.FullName -map 0:v:0 `
        -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le $out
    if ($LASTEXITCODE -ne 0) { Write-Warning "ffmpeg failed for $($file.Name)" }
}
