$ErrorActionPreference = 'Stop'
$content = Get-Content 'D:\SteamLibrary\steamapps\common\Fallout 4\fo4stereo.ini'
$content = $content -replace '^head_rot_compensate=1', 'head_rot_compensate=0'
$content = $content -replace '^aim_ratio=0.95', 'aim_ratio=0.85'
$content | Out-File 'D:\SteamLibrary\steamapps\common\Fallout 4\fo4stereo.ini' -Encoding UTF8
Write-Host 'Config updated in game directory.'
Get-Content 'D:\SteamLibrary\steamapps\common\Fallout 4\fo4stereo.ini' | Select-String 'compensate|head_rot|head_rotation|aim_ratio|pipboy_ratio'