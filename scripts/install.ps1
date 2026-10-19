param(
    # The portable OBS folder, and the profile whose saved settings the installer checks.
    [Parameter(Mandatory=$true)][string]$ObsRoot,
    [Parameter(Mandatory=$true)][string]$Profile,
    # An extracted release package (user ZIP) to install instead of this project's build.
    [string]$From
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$sourceRoot = if ($From) { (Resolve-Path -LiteralPath $From).Path } else { $projectRoot }
$ObsRoot = (Resolve-Path -LiteralPath $ObsRoot).Path
$exe = Join-Path $ObsRoot 'bin\64bit\obs64.exe'
if (!(Test-Path -LiteralPath (Join-Path $ObsRoot 'portable_mode.txt'))) { throw 'Target is not a portable OBS installation' }
$obsVersion = [version](((Get-Item -LiteralPath $exe).VersionInfo.FileVersion -split '\.')[0..2] -join '.')
if ($obsVersion -lt [version]'32.2.2') { throw "This build needs OBS 32.2.2 or newer; found $obsVersion" }
if ($obsVersion -ne [version]'32.2.2') { Write-Warning "OBS $obsVersion is newer than the tested 32.2.2. The plugin loads, and its Status box says it isn't tested with this version yet." }
$aitum = Join-Path $ObsRoot 'obs-plugins\64bit\vertical-canvas.dll'
if (Test-Path -LiteralPath $aitum) {
    $aitumVersion = $null
    try { $aitumVersion = [version](((Get-Item -LiteralPath $aitum).VersionInfo.FileVersion -split '\.')[0..2] -join '.') } catch { }
    if (!$aitumVersion -or $aitumVersion -lt [version]'1.6.6') { Write-Warning "Aitum Vertical $aitumVersion isn't supported (this build needs 1.6.6 or newer). Vertical recordings and Backtrack clips will get no extra tracks." }
    elseif ($aitumVersion -ne [version]'1.6.6') { Write-Warning "Aitum Vertical $aitumVersion is newer than the tested 1.6.6. Vertical recordings and Backtrack clips get extra tracks, and the Status box says this version isn't tested yet." }
}
if (@(Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }).Count) {
    throw 'Close the target portable OBS before replacing its loaded plugin DLL.'
}
if ($Profile -match '[\\/]|^\.{1,2}$') { throw 'Profile must be a profile directory name' }
$binary = Join-Path $sourceRoot 'build\obs-extra-audio.dll'
if (!(Test-Path -LiteralPath $binary)) { $binary = Join-Path $sourceRoot 'obs-plugins\64bit\obs-extra-audio.dll' }
if (!(Test-Path -LiteralPath $binary)) { throw 'Build the plugin first or use the release package' }
$helper = Join-Path $sourceRoot 'build\obs-extra-audio-mux.exe'
if (!(Test-Path -LiteralPath $helper)) { $helper = Join-Path $sourceRoot 'bin\64bit\obs-extra-audio-mux.exe' }
if (!(Test-Path -LiteralPath $helper)) { throw 'Build scripts/build-finalizer.ps1 or use the complete release package' }
$metadata = Join-Path $sourceRoot 'data/manifest.json'
if (!(Test-Path -LiteralPath $metadata)) { $metadata = Join-Path $sourceRoot 'data/obs-plugins/obs-extra-audio/manifest.json' }
if (!(Test-Path -LiteralPath $metadata)) { throw 'The plugin manager manifest is missing; use the complete release package.' }
$ini = Join-Path $ObsRoot "config\obs-studio\basic\profiles\$Profile\basic.ini"
if (!(Test-Path -LiteralPath $ini)) { throw "Profile not found: $Profile" }
$destination = Join-Path $ObsRoot 'obs-plugins\64bit\obs-extra-audio.dll'
$helperDestination = Join-Path $ObsRoot 'bin\64bit\obs-extra-audio-mux.exe'
$metadataDestination = Join-Path $ObsRoot 'data/obs-plugins/obs-extra-audio/manifest.json'
# Panel and status text, loaded by OBS from the module's data/locale folder.
$localeSource = Join-Path $sourceRoot 'data/locale'
if (!(Test-Path -LiteralPath $localeSource)) { $localeSource = Join-Path $sourceRoot 'data/obs-plugins/obs-extra-audio/locale' }
if (!(Test-Path -LiteralPath (Join-Path $localeSource 'en-US.ini'))) { throw 'The en-US locale file is missing; use the complete release package.' }
$localeDestination = Join-Path $ObsRoot 'data/obs-plugins/obs-extra-audio/locale'
$localeFiles = @(Get-ChildItem -LiteralPath $localeSource -Filter '*.ini' -File)
$backup = Join-Path $projectRoot ('backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
$manifest = @()
# manifest.json in the backup folder is this installation's own record, so
# the previous plugin metadata is saved under a different name.
$targets = @(@{Path=$destination; Name='obs-extra-audio.dll'},@{Path=$helperDestination; Name='obs-extra-audio-mux.exe'},@{Path=$metadataDestination; Name='plugin-manifest.json'})
$targets += @($localeFiles | ForEach-Object { @{Path=(Join-Path $localeDestination $_.Name); Name=('locale-' + $_.Name)} })
foreach ($entry in $targets) {
    $existed = Test-Path -LiteralPath $entry.Path
    $saved = Join-Path $backup $entry.Name
    if ($existed) { Copy-Item -LiteralPath $entry.Path -Destination $saved }
    $manifest += @{Path=$entry.Path; Backup=$saved; Existed=$existed}
}
try {
    Copy-Item -LiteralPath $binary -Destination $destination -Force
    Copy-Item -LiteralPath $helper -Destination $helperDestination -Force
    New-Item -ItemType Directory -Force -Path (Split-Path $metadataDestination -Parent) | Out-Null
    Copy-Item -LiteralPath $metadata -Destination $metadataDestination -Force
    New-Item -ItemType Directory -Force -Path $localeDestination | Out-Null
    foreach ($file in $localeFiles) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $localeDestination $file.Name) -Force }
    foreach ($entry in $manifest) { if (Test-Path -LiteralPath $entry.Path) { $entry.InstalledHash=(Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash } }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding utf8
} catch {
    foreach ($entry in $manifest) {
        if ($entry.Existed) { Copy-Item -LiteralPath $entry.Backup -Destination $entry.Path -Force }
        elseif (Test-Path -LiteralPath $entry.Path) { Remove-Item -LiteralPath $entry.Path }
    }
    throw
}
Write-Output "Installed: $destination"
Write-Output "Installed helper: $helperDestination"
Write-Output "Installed plugin manager metadata: $metadataDestination"
Write-Output "Installed text: $localeDestination ($($localeFiles.Count) language file(s))"
Write-Output 'Only plugin-owned files were installed. Profiles, scenes, and Aitum settings were preserved.'
Write-Output "Backup: $backup"
Write-Output 'Open Tools > Extra Audio Tracks to enable/name Tracks 7-16. Assign sources in Advanced Audio Properties.'
