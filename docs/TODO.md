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
| Head rotation reset after Pip-Boy | ✅ | Fixed 15.11: stale quaternion on camera state transition |

## What does NOT work yet

| Issue | Status | Debugging |
|---|---|---|
| **Aiming double vision** | ⏸ Blocked on RenderDoc diagnosis | ADS logs showed equal bounds and successful `Submit` calls, but both eyes received the same D3D texture object. Separate eye textures are implemented but need an in-game retest. The RenderDoc launch currently crashes before capture; isolate its interaction with Steam Overlay before trying again. See `docs/DEVLOG.md`. |
| **Camera looks slightly down-left** | 🔧 Fix built, needs test | Root cause candidate: OpenVR seated zero pose never resets pitch. New INI keys: `head_yaw_offset_deg`, `head_pitch_offset_deg`, `recenter_key` (F9 = 120). Press F9 while holding the head level: current head direction becomes the zero pose. |
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
| `aim_ipd_scale` | 0 | ADS camera-framing offset only; it does not enable mono rendering. Keep at 0 during diagnosis. |
| `aim_ratio` | 0.95 | FOV ratio threshold for aim detection |
| `pipboy_ratio` | 0.75 | FOV ratio threshold for Pip-Boy detection |
| `eye_delay` | 1 | Eye timing offset |
| `swap_eyes` | 0 | Set 1 if depth is inverted |
| `vr_cam_states` | 0 | Camera states shown in VR (0=1st person; 7=3rd person). Current default keeps 3rd person on the 2D screen. |
| `capture_aim_frames` | 0 | Save one left/right BMP pair for first-person hipfire and ADS; requires 1 and briefly stalls the GPU |
| `renderdoc_capture_key` | 122 (F11) | Trigger a one-frame RenderDoc `.rdc` capture; requires the RenderDoc launch script; 0 disables |
| `head_yaw_offset_deg` | 0 | Rotates the head-tracked view (and 2D screen) about world up |
| `head_pitch_offset_deg` | 0 | Tilts the head-tracked view (and 2D screen); fixes a view stuck slightly down/up |
| `recenter_key` | 120 (F9) | Press in game: current head direction becomes the new zero pose |

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

## HUD (planned, not started)

Päätökset 17.11.2026:
- Kaikki HUD-elementit säilytetään. Ei poisteta mitään oletuksena.
- HUD siirretään uskollisesti (faithful) uuteen sijaintiin, mutta elementtien suhteet säilyvät.
- Käyttäjä määrittää itse, mitä näkyy (MCM).

Tehtävät:
1. [x] **HUDMenu.swf-patchaustyökalu** (tehty: `tools/hud/`, testi-HUD asennettu): lukee käyttäjän oman pelin `Fallout4 - Interface.ba2`-arkiston,
   siirtää elementit (`HPMeter_mc`, `AmmoCount_mc`, `CompassWidget_mc`, `QuestMarkerHolder_mc`, `OtherMarkerHolder_mc`,
   `LocationText_mc`, `HUDActiveEffectsWidget_mc`, `RadsMeter_mc`, …) ja kirjoittaa tuloksen käyttäjän `Data\Interface`-kansioon.
   - Ei Bethesdan tiedostoja repoon. Repossa vain työkalu ja asetukset.
   - Työkalun pitää pystyä palauttamaan alkuperäinen (varmuuskopio).
2. [x] **Sijaintien mittaus** (tehty, tulokset `docs/HUD_PLAN.md` ja `docs/DEVLOG.md`): SWF:n `PlaceObject`-tagien matriisit ja elementtien ryhmät. Tarvitaan ennen siirtoa.
3. **MCM-konfiguraatiosivu (erillinen mod, asennetaan erikseen):**
   - Näkyvyys per elementti (päälle/pois).
   - Sijainti ja koko per elementti, ja sama säätö kuin proxyn säätötilassa.
   - Asetukset kirjoitetaan `fo4stereo.ini`-tiedostoon, jonka proxy lukee.
   - Ks. aiempi MCM-historia (`archive/retired-mcm`): asetusten ohitus ja seqlock-ongelma. Vältettävä: ini on lähde, MCM vain kirjoittaa siihen.
4. **Dialogi:** sama MCM-sivu sisältää keskustelupaneelin sijainnin ja koon.
5. **Testit:** jokaisen siirron jälkeen kuvakaappaus (`fo4stereo_shots`) ja tarkistus kypärästä.

## Next steps

1. **After the next machine restart, continue RenderDoc diagnosis** — do not relaunch the game through RenderDoc until its interaction with Steam Overlay is isolated. The current dump and observed stack are recorded in `docs/DEVLOG.md`.
2. **Verify the final ADS submissions** — with `aim_ipd_scale=0`, collect the transition logs for both `Submit` calls during one held ADS interval. Compare texture identities, bounds, eye sequence, zoom and camera state with hipfire before changing camera math.
3. **Test camera angle fix in game** — hold the head level and press F9 (`recenter_key`); or set `head_pitch_offset_deg`/`head_yaw_offset_deg` in fo4stereo.ini.
4. **Double rendering design decision** — see DEVLOG analysis; needs a decision before implementation.
5. **FRIK integration** — motion controllers, weapon offsets (future)

## Papyrus-natiivi kaatuu (18.11.2026)

- Kaatuminen heti, kun plugin rekisteröi `fo4stereo_hud.SetHud`-natiivin (`F4SE RegisterPapyrusFunctions` -> `PapyrusRegister` -> `BindNativeMethod`). Vedos `Fallout4.exe.38548.dmp`: kaatumispinossa plugin-koodin kutsu osoittaa pelin koodin keskelle (`push sp`).
- Tarkistettu: `BindNativeMethod` on samassa vtable-indeksissä (1B) ja NativeFunction/IVirtualMachine-REL::ID:t ovat samat kuin upstreamissa (`libxse/commonlibf4`). Kirjaston päivitys ei siis yksinään korjaa asiaa.
- **Testin tulos (18.11.):** parametriton no-op-natiivi (`fo4stereo_hud.Ping`) kaatui samalla tavalla, ilman ESP:tä. Kaatumisosoite on sama kuin ensimmäisessä testissä. Vika on siis rekisteröinnissä (`IVirtualMachine::BindNativeMethod`), ei funktion parametreissa. Papyrus-natiivi-reitti ei toimi tällä kirjastolla. Plugin palautettu pre-Papyrus-versioon (`2DC5A74F9C`).
- **Vaihtoehto ilman natiiveja:** Papyrus-skripti kirjoittaa arvot `Debug.Trace`-kutsulla Papyrus-lokiin, ja plugin tai proxy lukee lokin lopun. Vaatii `[Papyrus] bEnableLogging=1` -asetuksen (Fallout4Custom.ini). Ei F4SE-natiiveja, mutta lukeminen on viiveellinen.
- (Vanha suunnitelma) Seuraava testi: minimaalinen natiivi (ei parametreja, `RE::BSScript::IVirtualMachine::BindNativeMethod`) ilman ESP:n skriptiä. Jos sekin kaatuu, vika on rekisteröintitavassa (F4SE-papyrus-hook), ei funktion allekirjoituksessa.
- Turvallinen palautus: `fo4flat_probe.dll.bak_papyrus` (pre-Papyrus-versio), ESP poistettu `Plugins.txt`:stä.

## Päätös 17.11.2026 (tarkennus): modit

- Kolmansien osapuolten modeja ei käytetä.
- Omat ESP/ESM-tiedostot ovat sallittuja, jos ne ovat osa tätä projektia (ei lähdetä muiden tiedostoihin).
- Omien skriptien kääntäminen: `C:\Steam\steamapps\common\Fallout 4\Papyrus Compiler\PapyrusCompiler.exe` (Creation Kitin mukana).
- Tarvittava: arvojen lukeminen (HP, ammu) Papyrusin kautta ja välitys F4SE-plugin-natiivifunktiolla proxyyn.
- MCM:n tilalle: säätötila proxyssa (Numpad) + ini; näkyvyyskytkimet lisätään samaan säätötilaan, ei erillistä modia.
