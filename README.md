# F4-Flat-2-VR

Play the regular (flat) **Fallout 4** in a **VR headset** with real stereo 3D, head tracking and a gamepad.

> **Status: experimental, but playable.** Tested with Fallout 4 1.11.240 (Next-Gen, Steam), F4SE 0.7.9 and a
> Meta Quest 3 through SteamVR (Steam Link) on an RTX 4070 Ti. See [docs/DEVLOG.md](docs/DEVLOG.md) for the
> full development log (Finnish).

[![Latest Release](https://img.shields.io/github/v/release/kebabkenraali/F4-Flat-2-VR)](https://github.com/kebabkenraali/F4-Flat-2-VR/releases)

## Download

Grab the latest release from the [Releases page](https://github.com/kebabkenraali/F4-Flat-2-VR/releases) and extract the zip into your Fallout 4 folder.

## What works

| Feature | How |
|---|---|
| Stereo 3D in gameplay | The engine render camera is offset by IPD/2 per frame, so shadows, glass, lighting and effects stay correct |
| Output to the headset | Each frame goes to the matching eye texture and both eyes are submitted to SteamVR (OpenVR) |
| Head rotation and 6DoF position | HMD pose from SteamVR is applied to the game camera |
| Menus, intro, loading, Pip-Boy, VATS, dialogue | Shown on a virtual 2D screen in VR |
| First and third person | Rendered in VR |
| Any SteamVR headset | FOV measured at start, per-eye crop applied, window size and FOV written automatically |
| FSR1 upscale | Non-temporal spatial upscale to headset resolution (VR-safe) |
| MCM settings menu | In-game settings via pause > Mod Config (requires MCM mod) |

## Known issues

| Issue | Workaround |
|---|---|
| Aiming double vision | Check `fo4stereo_proxy.log` for `vr: zoom=1` when aiming. If zoom stays 0, lower `aim_ratio` in `fo4stereo.ini` |
| Camera slightly off-centre | Set `convergence_m=0` in `fo4stereo.ini` to disable toe-in yaw |
| Power armor exit animation stuck | Vanilla FO4 bug. Console: `player.moveto player` |
| Alternate-frame stereo | Each eye gets half the frame rate (inherent to current design) |
| No motion controllers | Gamepad only; FRIK integration planned |
| Physics tied to frame rate | Use 72 Hz headset refresh rate |

## Requirements

### Required

| Dependency | Version | Link |
|---|---|---|
| Fallout 4 (Steam) | 1.11.240 | [Steam](https://store.steampowered.com/app/377160/Fallout_4/) |
| F4SE (Script Extender) | 0.7.9 | [f4se.silverlock.org](https://f4se.silverlock.org/) |
| Address Library for F4SE Plugins | 1.11.240 | [Nexus Mods 47327](https://www.nexusmods.com/fallout4/mods/47327) |
| SteamVR | any | [Steam](https://store.steampowered.com/app/250820/SteamVR/) |
| PC VR headset | any SteamVR-compatible | e.g. Meta Quest 3 via Steam Link |

### Optional (in-game settings menu)

| Dependency | Version | Link |
|---|---|---|
| MCM (Mod Configuration Menu) | 21497 | [Nexus Mods 21497](https://www.nexusmods.com/fallout4/mods/21497) |
| MCM Settings Manager | 56195 | [Nexus Mods 56195](https://www.nexusmods.com/fallout4/mods/56195) |

Without MCM, settings are edited manually in `fo4stereo.ini`.

### To build from source

Visual Studio 2022 Build Tools (MSVC v143), CMake, Ninja and [vcpkg](https://github.com/microsoft/vcpkg).

## How it works

Two parts communicate through shared memory (`proxy/src/stereo_shm.h`):

| Part | Folder | Role |
|---|---|---|
| **F4SE plugin** `fo4flat_probe.dll` | `plugin/` | Finds the game PlayerCamera through MSVC RTTI (no address IDs, works across game versions) and hooks the camera states Update / GetTranslation / GetRotation. Applies the eye offset to the render camera node and the head rotation to the camera. |
| **D3D11 proxy** `d3d11.dll` | `proxy/` | Sits next to Fallout4.exe. Creates the game device on a DXGI 1.1 adapter (required by OpenVR), copies every presented frame into the current eye texture and submits both eyes to the SteamVR compositor. Also shows menus on a 2D overlay, reads the HMD pose, detects zoom (aiming, Pip-Boy) from the game projection, and auto-configures resolution and FOV. |

## Building

```bash
git clone --recursive https://github.com/kebabkenraali/F4-Flat-2-VR.git
cd F4-Flat-2-VR
git -C plugin/external/commonlibf4 apply ../../../patches/commonlibf4-winapi-msvc-14.44.patch
```

**Proxy:**
```bash
cmake -S proxy -B proxy/build -A x64
cmake --build proxy/build --config Release
```

**Plugin:** Use a 64-bit VS 2022 developer shell (required; CMake alone finds no standard headers):
```powershell
Import-Module "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" -DevCmdArguments "-arch=x64 -host_arch=x64" -SkipAutomaticLocation
cmake -S plugin -B plugin/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build plugin/b --config Release
```

Deploy: copy `d3d11.dll` next to `Fallout4.exe` and `fo4flat_probe.dll` into `Data\F4SE\Plugins\`.

## Configuration

All settings live in `fo4stereo.ini` next to `Fallout4.exe`. The most-used settings can also be changed in-game through the **Mod Configuration Menu** (requires the [MCM](https://www.nexusmods.com/fallout4/mods/21497) mod): pause menu > Mod Config > F4 Flat 2 VR. Stereo settings apply immediately; VR settings are pushed to the proxy through shared memory.

| Section | Key | Purpose |
|---|---|---|
| `[engine]` | `enabled`, `mode`, `ipd_units` | Engine-camera stereo: on/off, `stereo` / `wobble`, eye separation in game units (4.48 = 64 mm) |
| | `compensate`, `head_rotation`, `head_rot_compensate`, `head_position`, `units_per_meter`, `convergence_m`, `aim_ipd_scale` | Eye offset method; HMD rotation (0/1/2) and 6DoF; convergence distance; mono offset while aiming |
| `[vr]` | `enabled`, `eye_delay`, `swap_eyes`, `wait_poses` | Submit to SteamVR; eye timing; eye swap |
| | `flat_after_frames`, `screen_width_m`, `screen_distance_m` | Menus after N frames without camera updates; 2D screen size and distance in metres |
| | `fov_bounds`, `no_vsync`, `auto_configure` | Per-eye crop and automatic window size and FOV |
| | `aim_ratio`, `pipboy_ratio`, `pipboy_2d`, `other_cams_2d`, `vr_cam_states` | Zoom detection, and which camera states are shown in VR (0 first person, 7 third person) |
| | `upscale`, `upscale_sharpness` | FSR1 spatial upscale (non-temporal, VR-safe) |
| | `pipboy_projected`, `pipboy_target_index`, `pipboy_screen_width_m`, `pipboy_screen_distance_m`, `pipboy_screen_height_m` | Pip-Boy 2D overlay size and position |
| `[stereo]` | `enabled=0` | Old D3D matrix-patching experiment. Keep it off. |

## Tools used


- **Visual Studio 2022 Build Tools** (MSVC 14.44), **CMake**, **Ninja**, **vcpkg**
- **F4SE** (script extender runtime) and **Address Library for F4SE Plugins**
- **RenderDoc** for frame captures (research only)
- **SteamVR / OpenVR** and **Steam Link** with a Meta Quest 3 for testing
- **Git**, and **Cline** (Anthropic) as an AI pair programmer during development
- **AI-assisted development.** This project was developed with the help of AI tools (Cline / Claude, Anthropic). AI was used for code writing, debugging, documentation, packaging and testing guidance. All code was reviewed and tested by the author.

## Third-party code and credits

| What | Used how | License |
|---|---|---|
| [CommonLibF4](https://github.com/Ryan-rsm-McKenzie/CommonLibF4) by Ryan McKenzie | Git submodule, with a small patch for MSVC 14.44 (`patches/`) | MIT |
| [OpenVR](https://github.com/ValveSoftware/openvr) header `openvr.h` by Valve | Included unchanged in `proxy/external/openvr/` | BSD 3-Clause |
| F4SE `PluginAPI.h` | The `F4SEPluginVersionData` struct layout is reproduced in `plugin/src/main.cpp` so F4SE 0.7.x loads the plugin | F4SE own terms |
| [FRIK, Fallout 4 VR Body](https://github.com/rollingrock/Fallout-4-VR-Body) | **No code copied.** Source was read as a reference for how the VR game handles the camera root node. | - |
| [Cline](https://cline.bot) (Anthropic) | AI pair programmer: code fixes, documentation, FOMOD packaging, debugging | - |
- **AI-assisted development.** This project was developed with the help of AI tools (Cline / Claude, Anthropic). AI was used for code writing, debugging, documentation, packaging and testing guidance. All code was reviewed and tested by the author.

All other code in `plugin/src` and `proxy/src` was written for this project. Details are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE). You may use, modify and share this project for any purpose, including commercially, as long as you keep the license notice and **credit kebabkenraali** as the source.

This project contains no game files and is not affiliated with or endorsed by Bethesda Softworks or ZeniMax Media. Fallout is a trademark of ZeniMax Media.
