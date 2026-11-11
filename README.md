# F4-Flat-2-VR

Play the regular (flat) **Fallout 4** in a **VR headset** with real stereo 3D, head tracking and a gamepad.

> **Status: experimental, "but playable."** Tested with Fallout 4 1.11.240 (Next-Gen, Steam), F4SE 0.7.9 and a
> Meta Quest 3 through SteamVR (Steam Link) on an RTX 4070 Ti. See [docs/DEVLOG.md](docs/DEVLOG.md) for the
> full development log (Finnish).

## What works

| Feature | How |
|---|---|
| Stereo 3D in gameplay | The engine's own render camera is offset by ±IPD/2 per frame, so shadows, glass, lighting and effects stay correct |
| Output to the headset | Each frame goes to the matching eye texture and both eyes are submitted to SteamVR (OpenVR) |
| Head rotation (and position) | HMD pose from SteamVR is applied to the game's camera |
| Menus, intro, loading, Pip-Boy, VATS, dialogue | Shown on a virtual 2D screen in VR |
| First and third person | Rendered in VR |
| Aiming down sights | Both eyes get the same image (no double vision of the sights) |
| Any SteamVR headset | The headset's field of view is measured at start, each eye's view is cropped to match, and the game's window size and FOV are written automatically |

Known limitations:

- **Alternate-frame stereo.** The eyes take turns, so each eye gets half the frame rate.
- **No motion controllers.** Play with a gamepad (Xbox controller) or mouse and keyboard.
- **Window height is limited.** It cannot exceed your desktop height, which limits sharpness. NVIDIA DSR/DLDSR helps.
- **Physics are tied to frame rate.** Fallout 4's physics follow the frame rate. A 72 Hz headset refresh rate is a safe choice.

## How it works

Two parts talk through shared memory (`proxy/src/stereo_shm.h`):

| Part | Folder | Role |
|---|---|---|
| **F4SE plugin** `fo4flat_probe.dll` | `plugin/` | Finds the game's `PlayerCamera` through MSVC RTTI (no address IDs, works across game versions) and hooks the camera states' `Update` / `GetTranslation` / `GetRotation`. It applies the eye offset to the render camera node (`WorldRoot Camera`) and the head rotation to the camera. |
| **D3D11 proxy** `d3d11.dll` | `proxy/` | Sits next to `Fallout4.exe`. Creates the game's device on a DXGI 1.1 adapter (required by OpenVR), copies every presented frame into the current eye's texture and submits both eyes to the SteamVR compositor. It also shows menus on a 2D overlay, reads the HMD pose, detects zoom (aiming, Pip-Boy) from the game's projection, and auto-configures resolution and FOV. |

## Requirements

- Fallout 4 (Steam) **1.11.240**
- [F4SE](https://f4se.silverlock.org/) 0.7.9 and [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327) (1.11.240)
- SteamVR and a PC VR headset
- To build: Visual Studio 2022 Build Tools (MSVC v143), CMake, Ninja and [vcpkg](https://github.com/microsoft/vcpkg)

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

**Plugin.** Use a 64-bit VS 2022 developer shell and keep vcpkg on a short path (for example `C:\vcpkg`), because long paths break the compiler:
```powershell
Enter-VsDevShell -VsInstallPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" -DevCmdArguments "-arch=x64 -host_arch=x64"
cmake -S plugin -B plugin/b -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build plugin/b
```

## Installing

1. Copy `proxy/build/out/Release/d3d11.dll` to the Fallout 4 folder (next to `Fallout4.exe`).
2. Copy `plugin/b/fo4flat_probe.dll` to `Data\F4SE\Plugins\`.
3. Copy `config/fo4stereo.ini` to the Fallout 4 folder.
4. In `Documents\My Games\Fallout4\Fallout4Prefs.ini` set `sAntiAliasing=FXAA`. TAA smears with the per-eye camera.
5. In `Fallout4Custom.ini` under `[General]` add `bAlwaysActive=1`. Otherwise the game pauses when SteamVR takes window focus.
6. Start SteamVR and connect the headset. Reset the seated position while looking straight ahead. Then start the game through F4SE.
7. **First start with a new headset:** the proxy writes the window size and FOV for it. Restart the game once.

**Uninstall:** delete `d3d11.dll` and the plugin. To turn it off without deleting files, set
`[engine] enabled=0` and `[vr] enabled=0` in `fo4stereo.ini`.

## Configuration (`fo4stereo.ini`)

| Section | Key | Meaning |
|---|---|---|
| `[engine]` | `enabled`, `mode=stereo\|wobble` | Eye offset on the engine camera. `wobble` is a sideways sway test. |
| | `ipd_units` | Eye distance in game units. 4.48 = 64 mm (1 unit ≈ 1.43 cm). |
| | `compensate=4` | Eye offset method: 4 = render-camera node (works). 1 and 3 are older experiments. |
| | `head_rotation` (0/1/2), `head_rot_compensate`, `head_position`, `units_per_meter` | Head tracking |
| | `aim_ipd_scale` | Eye separation while aiming (0 = mono) |
| `[vr]` | `enabled`, `eye_delay`, `swap_eyes`, `wait_poses`, `no_vsync` | SteamVR output and frame timing |
| | `flat_after_frames`, `screen_width_m`, `screen_distance_m` | 2D screen for menus and loading |
| | `fov_bounds`, `auto_configure` | Per-eye crop and automatic window size and FOV |
| | `aim_ratio`, `pipboy_ratio`, `pipboy_2d`, `other_cams_2d`, `vr_cam_states` | Zoom detection, and which camera states are shown in VR (0 first person, 7 third person) |
| `[stereo]` | `enabled=0` | Old D3D matrix-patching experiment. Keep it off. |

## Tools used

- **Visual Studio 2022 Build Tools** (MSVC 14.44), **CMake**, **Ninja**, **vcpkg** (fmt, spdlog, Boost.STLInterfaces and the other CommonLibF4 dependencies)
- **F4SE** (script extender runtime) and **Address Library for F4SE Plugins**
- **RenderDoc** for frame captures and finding the camera constant buffers, plus **Python** scripts for analysing the dumps (research only, not shipped)
- **SteamVR / OpenVR** and **Steam Link** with a Meta Quest 3 for testing
- **Git**, and **Claude Code** (Anthropic) as an AI pair programmer during development

## Third-party code and credits

| What | Used how | License |
|---|---|---|
| [CommonLibF4](https://github.com/Ryan-rsm-McKenzie/CommonLibF4) by Ryan McKenzie | Git submodule, with a small patch for MSVC 14.44 (`patches/`) | MIT |
| [OpenVR](https://github.com/ValveSoftware/openvr) header `openvr.h` by Valve | Included unchanged in `proxy/external/openvr/` | BSD 3-Clause |
| F4SE `PluginAPI.h` | The `F4SEPluginVersionData` struct layout is reproduced in `plugin/src/main.cpp` so F4SE 0.7.x loads the plugin | F4SE's own terms |
| [FRIK, Fallout 4 VR Body](https://github.com/rollingrock/Fallout-4-VR-Body) | **No code copied.** Its source was read as a reference for how the VR game handles the camera root node. | — |

All other code in `plugin/src` and `proxy/src` was written for this project. Details are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE). You may use, modify and share this project for any purpose, including commercially, as long
as you keep the license notice and **credit kebabkenraali** as the source.

This project contains no game files and is not affiliated with or endorsed by Bethesda Softworks or ZeniMax
Media. Fallout is a trademark of ZeniMax Media.
