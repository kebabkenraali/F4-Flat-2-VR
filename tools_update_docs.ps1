$readme = 'C:\AI_Projektit\F4-Flat-2-VR\README.md'
$devlog = 'C:\AI_Projektit\F4-Flat-2-VR\docs\DEVLOG.md'

$addReadme = @'
## New fixes (13.11.2026)

- **FSR1 dispatch:** was 1D-layout `Dispatch(95912, 1)`, over D3D11's 65535 limit, fixed with 8x8 tiles `Dispatch(304, 316, 1)`.
- **EASU sampler:** was never bound (black output), fixed by creating a linear-clamp sampler and binding via `CSSetSamplers`.
- **RCAS:** read `midA` while still bound as UAV, fixed by unbinding UAV before SRV bind.
- **Mouse capture:** ClipCursor removed entirely; cursor now moves freely on desktop when game is backgrounded.
- **Aiming/Pip-Boy zoom:** `pipboy_ratio` 0.5 -> 0.75, `aim_ratio` 0.98 -> 0.95, `aim_ipd_scale=0` (mono); hysteresis holds until `aim_ratio + 0.015`.
- **Instrumentation:** proxy logs `vr: eye N bounds ...` on change; plugin logs `aim {}` and `conv yaw=... rad (... deg) eye=...`.
- **Build & deploy:** proxy and plugin rebuilt, d3d11.dll and fo4flat_probe.dll deployed to D:\SteamLibrary\steamapps\common\Fallout 4.

'@
Add-Content -Encoding UTF8 -Path $readme -Value $addReadme

$addDevlog = @'
## New fixes (13.11.2026)

- Mouse capture (ClipCursor) removed from proxy so Alt-Tab to desktop works.
- pipboy_ratio 0.5 -> 0.75, aim_ratio 0.98 -> 0.95, aim_ipd_scale=0 (mono).
- Instrumentation added: proxy logs vr: eye N bounds..., plugin logs aim{} conv yaw....
- Build & deploy: d3d11.dll and fo4flat_probe.dll deployed.

'@
Add-Content -Encoding UTF8 -Path $devlog -Value $addDevlog

Write-Output 'DOCS APPENDED OK'