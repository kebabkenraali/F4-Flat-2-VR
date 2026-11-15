<#
.Run against the game (not from here). Drop on the machine where the game is running,
then execute after "game loaded" (fo4flat_probe.log: "game data ready").

Usage:
  powershell -ExecutionPolicy Bypass -File C:/AI_Projektit/F4-Flat-2-VR/docs/test_steps.ps1
#>
$ErrorActionPreference = 'Stop'

function hdr($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }
function step($n,$t) { Write-Host "  [$n] $t" -ForegroundColor Yellow }
function ok($t) { Write-Host "  [ok] $t" -ForegroundColor Green }
function bad($t) { Write-Host "  [!] $t" -ForegroundColor Magenta }
function nah($t) { Write-Host "  [ ] $t" -ForegroundColor Gray }
function wait-log($file,$pat,$to=60) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while($sw.Elapsed.TotalSeconds -lt $to) {
    if(Test-Path $file) {
      if((Get-Content $file -Tail 20) -match $pat) { return $true }
    }
    Start-Sleep -Milliseconds 300
  }
  return $false
}
function show-log($file,$pat) {
  if(Test-Path $file) {
    Get-Content $file | Select-String $pat | Select-Object -First 10 | ForEach-Object {
      Write-Host "  $($_.LineNumber): $($_.Line)" -ForegroundColor White
    }
  } else { Write-Host "  [!] missing: $file" -ForegroundColor Magenta }
}

hdr "F4-Flat-2-VR test: stereo v2 + head tracking"
step 1 "Plugin log: $env:USERPROFILE\Documents\My Games\Fallout4\F4SE\fo4flat_probe.log"
step 2 "Proxy log: $env:USERPROFILE\Documents\My Games\Fallout4\F4SE\fo4stereo_proxy.log"
step 3 "Wait for 'game data ready'"

hdr "STEP A - stereo v2 camera eye shift (getTrans #)"
if(!(wait-log $env:USERPROFILE`+"\Documents\My Games\Fallout4\F4SE\fo4flat_probe.log" "getTrans #")) {
  bad "timed out waiting for getTrans #"; exit 1
}
show-log $env:USERPROFILE`+"\Documents\My Games\Fallout4\F4SE\fo4flat_probe.log" "getTrans #"
hdr "Expected: eye flips 0/1; out = orig + curApplied; spread ±2.24 (4.48/2) along right axis."
Write-Host "  If depth wrong -> swap_eyes=1. If eyes drift -> eye_delay=1." -ForegroundColor Yellow

hdr "STEP B - head rotation (getRot #)"
if(!(wait-log $env:USERPROFILE`+"\Documents\My Games\Fallout4\F4SE\fo4flat_probe.log" "getRot #")) {
  bad "timed out waiting for getRot #"; exit 1
}
show-log $env:USERPROFILE`+"\Documents\My Games\Fallout4\F4SE\fo4flat_probe.log" "getRot #"
hdr "Expected (head_rot_compensate=1): orig = raw state; out = cam * headQ."
Write-Host "  head_rot_compensate cancels prev-frame rotation -> out is fresh each frame." -ForegroundColor Yellow

hdr "STEP C - proxy submission (vr: frame ... submitOk/submitErr)"
if(!(wait-log $env:USERPROFILE`+"\Documents\My Games\Fallout4\F4SE\fo4stereo_proxy.log" "vr: frame")) {
  bad "timed out waiting for vr: frame"; exit 1
}
show-log $env:USERPROFILE`+"\Documents\My Games\Fallout4\F4SE\fo4stereo_proxy.log" "vr: frame"
hdr "Expected: submitOk rising, submitErr ~0; vr: mode switches 2D/3D stereo."
Write-Host "  If submitErr > 0, read error number (0=None, 106=SharedTexturesNotSupported)." -ForegroundColor Yellow

Write-Host "`nDone. Compare output against expectations above." -ForegroundColor Green
