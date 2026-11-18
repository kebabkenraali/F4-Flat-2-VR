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
| Configuration | `fo4stereo.ini` only; no MCM overrides or hidden live tuning |

## Known issues

| Issue | Workaround |
|---|---|
| Aiming double vision | Still needs an in-game retest. RenderDoc capture is temporarily blocked: the current launch path crashed before saving an `.rdc` file. Do not retry until Steam Overlay/RenderDoc interaction has been isolated; see `docs/DEVLOG.md`. |
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

All settings are edited manually in `fo4stereo.ini`. Changes are read at game or proxy startup; restart the game after changing them.

### To build from source

Visual Studio 2022 Build Tools (MSVC v143), CMake, Ninja and [vcpkg](https://github.com/microsoft/vcpkg). RenderDoc's `renderdoc_app.h` is optional at build time; when available from a RenderDoc installation, it enables the in-game F11 capture API.

### Capturing a RenderDoc frame

The F11 capture workflow is implemented, but is currently **not safe to run**: the latest attempt crashed Fallout 4 before producing a capture. The crash stack contains both `gameoverlayrenderer64.dll` and RenderDoc/DXGI calls even though the per-game Steam Overlay setting was reported disabled. This does not prove which module caused the crash. See `docs/DEVLOG.md` for the dump path and next diagnostic steps. Do not run `capture_renderdoc.ps1` again until the overlay interaction has been isolated.

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

All runtime settings come from `fo4stereo.ini` next to `Fallout4.exe`; there is no MCM reader or second settings file. For ADS debugging, keep `aim_ipd_scale=0` unless a specific camera-position test calls for changing it: this offset changes framing, not whether the image is mono or stereo.

| Section | Key | Purpose |
|---|---|---|
| `[engine]` | `enabled`, `mode`, `ipd_units`, `use_runtime_ipd` | Engine-camera stereo: on/off, `stereo` / `wobble`, manual eye separation in game units (4.48 = 64 mm), or headset IPD when available |
| | `compensate`, `head_rotation`, `head_rot_compensate`, `head_position`, `units_per_meter`, `convergence_m`, `aim_ipd_scale` | Eye offset method; HMD rotation (0/1/2) and 6DoF; convergence distance; signed ADS camera-framing offset (0 = no extra offset) |
| `[vr]` | `enabled`, `eye_delay`, `swap_eyes`, `wait_poses` | Submit to SteamVR; eye timing; eye swap |
| | `flat_after_frames`, `screen_width_m`, `screen_distance_m` | Menus after N frames without camera updates; 2D screen size and distance in metres |
| | `fov_bounds`, `no_vsync`, `auto_configure` | Per-eye crop and automatic window size and FOV |
| | `capture_aim_frames` | When 1, automatically save left/right BMP pairs for first-person hipfire and ADS; disabled by default |
| | `renderdoc_capture_key` | Virtual-key code for one-frame RenderDoc captures; default 122 (F11), requires launching via `capture_renderdoc.ps1`; 0 disables |
| | `aim_ratio`, `pipboy_ratio`, `pipboy_2d`, `other_cams_2d`, `vr_cam_states` | Zoom detection, and which camera states are shown in VR (0 first person, 7 third person) |
| | `hud_panels`, `hud_mask`, `hud_bg_alpha`, `hud_*_crop`, `hud_*_yaw_deg`, `hud_*_pitch_deg`, `hud_*_width_m` | HP, ammo and compass drawn as head-locked panels; the game HUD is masked out of the world. Needs `fHUDOpacity=1` in Fallout4Prefs.ini. Numpad 5 opens the on-screen tuning mode |
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

## HUD and in-game tuning (18.11.2026)

- **HUD frame:** the area inside which all HUD elements must fit is set by four screen edges in degrees
  from the view centre: `hud_frame_left_deg`, `hud_frame_right_deg`, `hud_frame_top_deg`, `hud_frame_bottom_deg`
  (defaults -50, 50, 20, -30). Elements are placed inside the frame using their normalised positions
  (`hud_<name>_fx`, `hud_<name>_fy`) and kept inside the edges.
- **Tuning mode (Numpad 5):** Numpad 7 switches between frame, element and crop modes. In frame mode
  Numpad 4/6/8/2 move the frame edges, Numpad 1/9 select, Numpad 3 toggles the element. Numpad +/- zoom the
  whole HUD, Numpad 0 saves the frame edges and element positions to the settings file.
- **MCM:** `mo2/F4Flat2VR_Dev/MCM` contains the MCM page configuration (HUD frame, dialogue panel and
  general settings). The MCM settings file `Data\MCM\Settings\F4Flat2VR.ini` overrides values from `fo4stereo.ini`.
- **HUD file:** `tools/hud/` extracts the game's `HUDMenu.swf` from your own installation (read only),
  reports element positions (`swf_layout.py`), and fits the HUD into the frame (`swf_fit.py`).
  Bethesda's files are not included in this repository.
- No third-party UI mods are required. Two mods that replace `HUDMenu.swf` cannot be active at the same time.
