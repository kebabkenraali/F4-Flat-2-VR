# Deploy the F4 Flat 2 VR MCM menu to the game's Data folder.
# The menu files go to Data\MCM\Config\F4Flat2VR\ (user settings are written by
# the MCM core to Data\MCM\Settings\F4Flat2VR.ini once the menu is used).

$game = 'D:\SteamLibrary\steamapps\common\Fallout 4'
$src = Join-Path $PSScriptRoot 'mcm\F4Flat2VR'
$dst = Join-Path $game 'Data\MCM\Config\F4Flat2VR'

if (-not (Test-Path $src)) {
    Write-Error "Source not found: $src"
    exit 1
}

New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item -Force (Join-Path $src 'config.json') $dst
Copy-Item -Force (Join-Path $src 'settings.ini') $dst
Write-Output "MCM menu deployed to $dst"
