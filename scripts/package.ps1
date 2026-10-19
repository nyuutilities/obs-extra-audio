$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$metadataPath = Join-Path $projectRoot 'data/manifest.json'
$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
$name = "obs-extra-audio-$($metadata.version)"
$dist = Join-Path $projectRoot 'dist'
# The README ships in both packages, and its heading names the version.
$heading = Get-Content -LiteralPath (Join-Path $projectRoot 'README.md') -TotalCount 1 -Encoding UTF8
if ($heading -ne "# OBS Extra Audio $($metadata.version)") { throw "README.md's heading ('$heading') doesn't name version $($metadata.version)." }

# User package: only the files OBS loads, in OBS's own folder layout, so it can be extracted straight
# into an OBS folder. Its README, license and checksums sit in the plugin's data folder, not OBS's main folder.
$releaseZip = Join-Path $dist "$name-windows-x64.zip"
$sourceZip = Join-Path $dist "$name-source.zip"
foreach ($path in @($releaseZip, $sourceZip)) {
    if (Test-Path -LiteralPath $path) { throw "Already packaged: $path. Move the old package aside first." }
}
# Staged under build\ and removed after zipping, so dist\ holds only the ZIPs.
$release = Join-Path $projectRoot "build\package-user\$name-windows-x64"
if (Test-Path -LiteralPath $release) { Remove-Item -LiteralPath $release -Recurse -Force }  # this script's own staging copy
$data = Join-Path $release 'data\obs-plugins\obs-extra-audio'
foreach ($path in @((Join-Path $release 'obs-plugins\64bit'), (Join-Path $release 'bin\64bit'), (Join-Path $data 'locale'))) {
    New-Item -ItemType Directory -Force -Path $path | Out-Null
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'build\obs-extra-audio.dll') -Destination (Join-Path $release 'obs-plugins\64bit\obs-extra-audio.dll')
Copy-Item -LiteralPath (Join-Path $projectRoot 'build\obs-extra-audio-mux.exe') -Destination (Join-Path $release 'bin\64bit\obs-extra-audio-mux.exe')
Copy-Item -LiteralPath $metadataPath -Destination (Join-Path $data 'manifest.json')
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'data/locale') -Filter '*.ini' -File | Copy-Item -Destination (Join-Path $data 'locale')
foreach ($file in @('README.md', 'LICENSE')) { Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $data }
$sums = Get-ChildItem -LiteralPath $release -Recurse -File | ForEach-Object {
    "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)  $($_.FullName.Substring($release.Length + 1).Replace('\', '/'))"
}
$sums | Set-Content -LiteralPath (Join-Path $data 'SHA256SUMS.txt') -Encoding ascii
Compress-Archive -Path (Join-Path $release '*') -DestinationPath $releaseZip
Remove-Item -LiteralPath (Split-Path $release -Parent) -Recurse -Force

# Source package: everything needed to build and install the plugin, the same files as the public
# repository. The plugin is GPL, so anyone given the user package must also be able to get this.
$staging = Join-Path $projectRoot "build\package-source\$name-source"
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }  # this script's own staging copy
New-Item -ItemType Directory -Force -Path $staging | Out-Null
foreach ($folder in @('src', 'scripts')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $staging $folder) | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $projectRoot $folder) -File | Copy-Item -Destination (Join-Path $staging $folder)
}
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'data\locale') | Out-Null
Copy-Item -LiteralPath $metadataPath -Destination (Join-Path $staging 'data')
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'data/locale') -Filter '*.ini' -File | Copy-Item -Destination (Join-Path $staging 'data\locale')
foreach ($file in @('README.md', 'DEVELOPMENT.md', 'LICENSE', '.gitignore')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $staging
}
Compress-Archive -LiteralPath $staging -DestinationPath $sourceZip
Remove-Item -LiteralPath (Split-Path $staging -Parent) -Recurse -Force

Write-Output "User package: $releaseZip (extract into the OBS folder)"
Write-Output "Source package: $sourceZip"
