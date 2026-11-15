# TODO — F4-Flat-2-VR

## What works (tested)

| Feature | Status | Notes |
|---|---|---|
| Stereo 3D in gameplay | ✅ | Engine-camera eye shift, alternating frames |
| Head rotation tracking | ✅ | `head_rotation=1`, camera×head |
| Head position (6DoF) | ✅ | `head_position=1` |
| Menus / intro / loading on 2D screen | ✅ | `flat_after_frames=20` |
| Pip-Boy on 2D screen | ✅ | `pipboy_2d=1` |
| VATS / dialogue on 2D screen | ✅ | `other_cams_2d=1` |
| FOV auto-configuration | ✅ | `auto_configure=1` writes Fallout4Custom.ini |
| FSR1 upscale | ✅ | Non-temporal, VR-safe |
| MCM settings menu | ✅ | Polls `Data\MCM\Settings\F4Flat2VR.ini` every 500 ms |
| Head rotation reset after Pip-Boy | ✅ | Fixed 15.11: stale quaternion on camera state transition |

## What does NOT work yet

| Issue | Status | Debugging |
|---|---|---|
| **Aiming double vision** | ❌ | `aim_ratio=0.95` may not trigger. Check `fo4stereo_proxy.log` for `vr: zoom=1` when ADS. If zoom stays 0, lower `aim_ratio` toward 0.85. |
| **Camera looks slightly down-left** | ❌ | Likely `convergence_m=1.5` toe-in yaw. Test with `convergence_m=0` to isolate. |
| **Power armor exit animation stuck** | ❌ | Vanilla FO4 bug, not caused by this mod. Console: `player.moveto player`. |
| **Double rendering (both eyes same frame)** | ❌ | Requires per-frame double render; currently alternate-frame stereo. |
| **Motion controllers** | ❌ | Gamepad only; FRIK integration planned. |

## Config reference (`fo4stereo.ini`)

| Key | Value | Purpose |
|---|---|---|
| `compensate` | 1 | Eye offset method (1=delta, 4=child offset) |
| `head_rotation` | 1 | 0=off, 1=camera×head, 2=head×camera |
| `head_rot_compensate` | 0 | 1=remove prev frame rotation (causes issues) |
| `head_position` | 1 | 6DoF head position |
| `units_per_meter` | 70 | FO4 units per metre |
| `convergence_m` | 1.5 | Toe-in distance; **set 0 if camera looks off-centre** |
| `aim_ipd_scale` | 0 | Mono when aiming (0=centre) |
| `aim_ratio` | 0.95 | FOV ratio threshold for aim detection |
| `pipboy_ratio` | 0.75 | FOV ratio threshold for Pip-Boy detection |
| `eye_delay` | 1 | Eye timing offset |
| `swap_eyes` | 0 | Set 1 if depth is inverted |
| `vr_cam_states` | 0,7 | Camera states shown in VR (0=1st person, 7=3rd person) |

## Build & deploy

```powershell
# Plugin (requires VS DevShell)
Import-Module 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Enter-VsDevShell -VsInstallPath 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' -DevCmdArguments '-arch=x64 -host_arch=x64' -SkipAutomaticLocation
cmake --build plugin/b --config Release

# Proxy
cmake --build proxy/build --config Release

# Deploy
Copy-Item plugin\b\fo4flat_probe.dll 'D:\SteamLibrary\steamapps\common\Fallout 4\Data\F4SE\Plugins\'
Copy-Item proxy\build\out\Release\d3d11.dll 'D:\SteamLibrary\steamapps\common\Fallout 4\'
Copy-Item config\fo4stereo.ini 'D:\SteamLibrary\steamapps\common\Fallout 4\'
```

## Next steps

1. **Fix aiming** — check proxy log for `vr: zoom=` value during ADS; adjust `aim_ratio`
2. **Fix camera angle** — test `convergence_m=0` to isolate toe-in yaw
3. **Stereo v2 verification** — check `getTrans #` and `getRot #` in plugin log
4. **Double rendering** — render both eyes per frame (future)
5. **FRIK integration** — motion controllers, weapon offsets (future)
