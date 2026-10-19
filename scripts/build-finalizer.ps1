param([Parameter(Mandatory=$true)][string]$ObsRoot)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$build = Join-Path $projectRoot 'build'
$ffmpegSource = Join-Path $projectRoot '.deps\FFmpeg-n8.0'
$zig = Join-Path $projectRoot '.deps\zig-x86_64-windows-0.14.1\zig.exe'
if (!(Test-Path -LiteralPath (Join-Path $ffmpegSource 'libavformat\avformat.h'))) { throw 'Official FFmpeg n8.0 headers are required' }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build 'zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build 'zig-local-cache'
$generated = Join-Path $build 'ffmpeg-include\libavutil'
New-Item -ItemType Directory -Path $generated -Force | Out-Null
@'
#pragma once
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
'@ | Set-Content -LiteralPath (Join-Path $generated 'avconfig.h') -Encoding ascii
$libraries = @()
foreach ($library in @('avformat-62','avcodec-62','avutil-60')) {
    $definition = Join-Path $build "$library.def"
    $import = Join-Path $build "$library.lib"
    python (Join-Path $PSScriptRoot 'pe_exports.py') (Join-Path $ObsRoot "bin\64bit\$library.dll") $definition
    if ($LASTEXITCODE) { throw 'FFmpeg export extraction failed' }
    & $zig dlltool -m i386:x86-64 -d $definition -l $import
    if ($LASTEXITCODE) { throw 'FFmpeg import library creation failed' }
    $libraries += $import
}
& $zig cc -target x86_64-windows-gnu -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations -I (Join-Path $projectRoot '.deps\obs-studio-32.2.2\libobs') -I $build -I (Join-Path $build 'ffmpeg-include') -I $ffmpegSource -I (Join-Path $projectRoot 'src') (Join-Path $projectRoot 'src\mp4-finalizer.c') (Join-Path $projectRoot 'src\mp4-inplace.c') (Join-Path $build 'obs.lib') @libraries -o (Join-Path $build 'obs-extra-audio-mux.exe')
if ($LASTEXITCODE) { throw 'Finalizer build failed' }
Write-Output 'Built the MP4 finalizer against the OBS-bundled FFmpeg DLLs.'
