$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$deps = Join-Path $projectRoot '.deps'
New-Item -ItemType Directory -Path $deps -Force | Out-Null
# Each dependency is downloaded and unpacked only when its unpacked copy is missing,
# so a machine that already has .deps never downloads anything.
function Get-PinnedSource([string]$Marker, [string]$File, [string]$Url, [string]$Sha256, [string]$Name) {
    if (Test-Path -LiteralPath (Join-Path $deps $Marker)) { return }
    $archive = Join-Path $deps $File
    if (!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest $Url -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Sha256) { throw "$Name archive checksum mismatch" }
    Expand-Archive -LiteralPath $archive -DestinationPath $deps
}
Get-PinnedSource 'zig-x86_64-windows-0.14.1\zig.exe' 'zig.zip' 'https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip' '554F5378228923FFD558EAC35E21AF020C73789D87AFEABF4BFD16F2E6FEED2C' 'Zig'
Get-PinnedSource 'obs-studio-32.2.2\libobs\obs.h' 'obs-source.zip' 'https://github.com/obsproject/obs-studio/archive/refs/tags/32.2.2.zip' 'F15F001F1FA526405318835F44F9910046502F496EBC3A30D5296A5018B831AA' 'OBS source'
Get-PinnedSource 'FFmpeg-n8.0\libavformat\avformat.h' 'ffmpeg-n8.0.zip' 'https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n8.0.zip' '775DB720F4821967C360B7CECB277540A9EB8D72A9AF1CD7C1D2849C4C8C881C' 'FFmpeg source'
Write-Output 'Pinned Zig 0.14.1, OBS 32.2.2 and FFmpeg 8.0 headers are ready.'
