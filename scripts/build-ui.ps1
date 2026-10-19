param([Parameter(Mandatory=$true)][string]$ObsRoot)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$build=Join-Path $projectRoot 'build'
$metadataPath=Join-Path $projectRoot 'data/manifest.json'
if (!(Test-Path -LiteralPath $metadataPath)) { $metadataPath=Join-Path $projectRoot 'data/obs-plugins/obs-extra-audio/manifest.json' }
$metadata=Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
$header=Get-Content -LiteralPath (Join-Path $projectRoot 'src/routing.h') -Raw
if ($header -notmatch ('#define EA_VERSION "'+[regex]::Escape($metadata.version)+'"')) { throw 'Plugin metadata and source version differ.' }
New-Item -ItemType Directory -Force -Path $build | Out-Null
@'
#pragma once
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@ | Set-Content -LiteralPath (Join-Path $build 'obsconfig.h') -Encoding ascii
$toolRoot=Join-Path $projectRoot '.deps/msvc-14.44/Contents/VC/Tools/MSVC/14.44.35207'
$qtRoot=Join-Path $projectRoot '.deps/qt-6.11.1'
$sdkRoot='C:/Program Files (x86)/Windows Kits/10'
$sdkVer='10.0.19041.0'
$cl=Join-Path $toolRoot 'bin/Hostx64/x64/cl.exe'
$link=Join-Path $toolRoot 'bin/Hostx64/x64/link.exe'
if (!(Test-Path -LiteralPath $cl) -or !(Test-Path -LiteralPath "$qtRoot/include/QtCore/QObject")) { throw 'Run scripts/bootstrap-ui.ps1 for the pinned native Qt build dependencies.' }
$zig=Join-Path $projectRoot '.deps/zig-x86_64-windows-0.14.1/zig.exe'
foreach ($library in @('obs','obs-frontend-api','avformat-62','avcodec-62','avutil-60')) {
 python (Join-Path $PSScriptRoot 'pe_exports.py') (Join-Path $ObsRoot "bin/64bit/$library.dll") (Join-Path $build "$library.def")
 if ($LASTEXITCODE) { throw 'Export extraction failed' }
 & $zig dlltool -m i386:x86-64 -d (Join-Path $build "$library.def") -l (Join-Path $build "$library.lib")
 if ($LASTEXITCODE) { throw 'Import library creation failed' }
}
$ffmpegSource=Join-Path $projectRoot '.deps/FFmpeg-n8.0'
if (!(Test-Path -LiteralPath (Join-Path $ffmpegSource 'libavformat/avformat.h'))) { throw 'Run scripts/bootstrap.ps1 for the pinned FFmpeg headers.' }
$generated=Join-Path $build 'ffmpeg-include/libavutil'
New-Item -ItemType Directory -Force -Path $generated | Out-Null
@'
#pragma once
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
'@ | Set-Content -LiteralPath (Join-Path $generated 'avconfig.h') -Encoding ascii
$savedInclude=$env:INCLUDE
try {
 $env:INCLUDE="$toolRoot/include;$sdkRoot/Include/$sdkVer/ucrt;$sdkRoot/Include/$sdkVer/shared;$sdkRoot/Include/$sdkVer/um;$qtRoot/include;$qtRoot/include/QtCore;$qtRoot/include/QtGui;$qtRoot/include/QtWidgets"
 $includes=@((Join-Path $projectRoot '.deps/obs-studio-32.2.2/libobs'),(Join-Path $projectRoot '.deps/obs-studio-32.2.2/frontend/api'),$build,(Join-Path $projectRoot 'src')) | ForEach-Object { "/I$_" }
 $includes+=@((Join-Path $projectRoot '.deps/FFmpeg-n8.0'),(Join-Path $build 'ffmpeg-include')) | ForEach-Object { "/I$_" }
 $includes+=('/I'+(Join-Path $projectRoot '.deps/obs-studio-32.2.2/plugins/obs-ffmpeg'))
 $includes+=('/I'+(Join-Path $projectRoot '.deps/obs-studio-32.2.2/deps/w32-pthreads'))
 $includes+=('/I'+(Join-Path $projectRoot '.deps/obs-studio-32.2.2/deps/libcaption'))
 $includes+=('/I'+(Join-Path $projectRoot '.deps/uthash-2.3.0'))
 # Source paths that headers record (Qt's, for example) start at this folder, so the DLL doesn't name where it was built.
 $trim="/d1trimfile:$projectRoot"
 $objects=@()
 foreach ($source in @('plugin','routing','source-recorder','recording-manager','replay-manager','encoder-lane','native-pause-clock','track-config','ui-bridge','aac-recorder','mp4-clock','mp3-backup')) {
  $object=Join-Path $build "$source-native.obj"
  & $cl /nologo /TC /std:c11 /experimental:c11atomics /utf-8 /O2 /MD /W3 /we4013 /wd5105 /D_CRT_SECURE_NO_WARNINGS $trim /c @includes "/Fo$object" (Join-Path $projectRoot "src/$source.c")
  if ($LASTEXITCODE) { throw "Native C build failed: $source" }; $objects+=$object
 }
 $uiObject=Join-Path $build 'qt-ui.obj'
 & $cl /nologo /TP /std:c++17 /Zc:__cplusplus /permissive- /utf-8 /O2 /MD /EHsc /W3 /we4013 $trim /c @includes "/Fo$uiObject" (Join-Path $projectRoot 'src/qt-ui.cpp')
 if ($LASTEXITCODE) { throw 'Qt UI build failed' }; $objects+=$uiObject
 & $link /nologo /DLL "/OUT:$(Join-Path $build 'obs-extra-audio.dll')" @objects (Join-Path $build 'obs.lib') (Join-Path $build 'obs-frontend-api.lib') (Join-Path $build 'avformat-62.lib') (Join-Path $build 'avcodec-62.lib') (Join-Path $build 'avutil-60.lib') "$qtRoot/lib/Qt6Widgets.lib" "$qtRoot/lib/Qt6Gui.lib" "$qtRoot/lib/Qt6Core.lib" "/LIBPATH:$toolRoot/lib/spectre/x64" "/LIBPATH:$toolRoot/lib/x64" "/LIBPATH:$sdkRoot/Lib/$sdkVer/ucrt/x64" "/LIBPATH:$sdkRoot/Lib/$sdkVer/um/x64" msvcprt.lib msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib user32.lib version.lib
 if ($LASTEXITCODE) { throw 'Native plugin link failed' }
 Write-Output 'Built the single plugin DLL with the native Qt UI and C11 audio backend.'
} finally { $env:INCLUDE=$savedInclude }

