$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$deps = Join-Path $projectRoot '.deps'
$sevenZip = 'C:/Program Files/7-Zip/7z.exe'
if (!(Test-Path -LiteralPath $sevenZip)) { throw 'Install 7-Zip, or update $sevenZip to its existing location.' }
$sdk = 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0'
if (!(Test-Path -LiteralPath "$sdk/um/Windows.h")) { throw 'Windows SDK 10.0.19041.0 is required for the native build.' }
New-Item -ItemType Directory -Force -Path $deps | Out-Null
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ui-dependencies.json') -Raw | ConvertFrom-Json
function Get-PinnedArchive($Entry) {
    $archive = Join-Path $deps $Entry.file
    if (!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $Entry.url -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Checksum mismatch: $($Entry.file)" }
    return $archive
}
function Expand-PinnedArchive([string]$Archive, [string]$Destination) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    & $sevenZip x -y -aos "-o$Destination" $Archive | Out-Null
    if ($LASTEXITCODE) { throw "Archive extraction failed: $Archive" }
}
# Each dependency is downloaded and unpacked only when its unpacked copy is missing,
# so a machine that already has .deps never downloads anything.
$qtRoot = Join-Path $deps 'qt-6.11.1'
if (!(Test-Path -LiteralPath "$qtRoot/include/QtCore/QObject")) { Expand-PinnedArchive (Get-PinnedArchive $manifest.qt) $qtRoot }
$msvcRoot = Join-Path $deps 'msvc-14.44'
# One file from each of the four pinned compiler packages, in manifest order.
$msvcTools = Join-Path $msvcRoot 'Contents/VC/Tools/MSVC/14.44.35207'
$msvcReady = @('include/vcruntime.h','lib/spectre/x64/msvcrt.lib','bin/Hostx64/x64/cl.exe','bin/Hostx64/x64/1033/clui.dll') |
    ForEach-Object { Test-Path -LiteralPath (Join-Path $msvcTools $_) }
if ($msvcReady -contains $false) { foreach ($entry in $manifest.msvc) { Expand-PinnedArchive (Get-PinnedArchive $entry) $msvcRoot } }
$uthashRoot = Join-Path $deps 'uthash-2.3.0'
if (!(Test-Path -LiteralPath (Join-Path $uthashRoot 'uthash.h'))) {
    New-Item -ItemType Directory -Force -Path $uthashRoot | Out-Null
    Copy-Item -LiteralPath (Get-PinnedArchive $manifest.uthash) -Destination (Join-Path $uthashRoot 'uthash.h') -Force
}
Write-Output 'Pinned Qt 6.11.1 and MSVC 14.44 are ready inside .deps. No system compiler or Qt installation was changed.'
