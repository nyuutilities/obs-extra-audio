param([Parameter(Mandatory=$true)][string]$Manifest)
$ErrorActionPreference = 'Stop'
$entries = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
foreach ($entry in $entries) {
    if ($entry.InstalledHash -and (Test-Path -LiteralPath $entry.Path) -and
        (Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash -ne $entry.InstalledHash) {
        throw "File changed since installation; restore the needed settings from backup manually: $($entry.Path)"
    }
    if ($entry.Path -match '^(.*)[\\/]obs-plugins[\\/]64bit[\\/]obs-extra-audio\.dll$') {
        $exe = Join-Path $Matches[1] 'bin\64bit\obs64.exe'
        if (@(Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe}).Count) { throw 'Close OBS first' }
    }
}
foreach ($entry in $entries) {
    if ($entry.Existed) { Copy-Item -LiteralPath $entry.Backup -Destination $entry.Path -Force }
    elseif (($entry.Path -match '[\\/](obs-extra-audio\.dll|obs-extra-audio-mux\.exe)$' -or
             $entry.Path -match '[\\/]data[\\/]obs-plugins[\\/]obs-extra-audio[\\/](manifest\.json|locale[\\/][^\\/]+\.ini)$') -and
            (Test-Path -LiteralPath $entry.Path)) { Remove-Item -LiteralPath $entry.Path }
}
Write-Output 'Plugin-owned files restored or removed using the installation backup.'
