param([Parameter(Mandatory=$true)][string]$ObsRoot)
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot 'build-ui.ps1') -ObsRoot $ObsRoot
& (Join-Path $PSScriptRoot 'build-finalizer.ps1') -ObsRoot $ObsRoot
