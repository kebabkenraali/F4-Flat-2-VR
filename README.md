# F4-Flat-2-VR

Tavallinen (flat) **Fallout 4** pelattavaksi **VR-laseilla** aidolla stereokuvalla ja peliohjaimella.

> **Tila: kokeellinen, kehitys käynnissä.** Silmäsiirto pelimoottorin kameraan toimii, ja kuva välittyy SteamVR:n
> kautta kypärään. Silmien erottelu, 2D-näkymä valikoille ja latausruuduille sekä pään seuranta (6DoF) ovat työn alla.
> Katso [docs/DEVLOG.md](docs/DEVLOG.md).

*English summary below.*

## Miten se toimii

Projekti koostuu kahdesta osasta, jotka keskustelevat jaetun muistin kautta (`proxy/src/stereo_shm.h`):

| Osa | Kansio | Tehtävä |
|---|---|---|
| **F4SE-plugin** `fo4flat_probe.dll` | `plugin/` | Etsii pelin `PlayerCamera`-olion RTTI:n avulla (versiosta riippumatta) ja koukuttaa kameratilojen `GetTranslation`-funktion. Lisää kameran paikkaan silmäsiirron ±IPD/2, jolloin moottori laskee kaikki matriisit, varjot ja lasin itse oikein. |
| **D3D11-proxy** `d3d11.dll` | `proxy/` | Asentuu pelikansioon. Kopioi jokaisen valmiin kehyksen kyseisen silmän tekstuuriin ja lähettää molemmat silmät SteamVR:n compositorille (OpenVR). Luo pelin laitteen DXGI 1.1 -adapterilla, jota OpenVR vaatii. |

Silmät vuorottelevat kehyksittäin (alternate-frame stereo).

## Vaatimukset

- Fallout 4, Steam-versio **1.11.240** (Next-Gen)
- [F4SE](https://f4se.silverlock.org/) 0.7.9 ja Address Library for F4SE Plugins (versio 1.11.240)
- SteamVR ja PC VR -kypärä (testattu: Meta Quest 3, Steam Link)
- Kääntämiseen: Visual Studio 2022 Build Tools (MSVC v143), CMake, Ninja ja [vcpkg](https://github.com/microsoft/vcpkg)

## Kääntäminen

```bash
git clone --recursive https://github.com/kebabkenraali/F4-Flat-2-VR.git
cd F4-Flat-2-VR
git -C plugin/external/commonlibf4 apply ../../../patches/commonlibf4-winapi-msvc-14.44.patch
```

**Proxy** (Visual Studio -generaattori):
```bash
cmake -S proxy -B proxy/build -A x64
cmake --build proxy/build --config Release
```

**Plugin** (64-bittinen VS 2022 -kehittäjäympäristö, Ninja, vcpkg lyhyessä polussa, esim. `C:\vcpkg`):
```powershell
Enter-VsDevShell -VsInstallPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" -DevCmdArguments "-arch=x64 -host_arch=x64"
cmake -S plugin -B plugin/b -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build plugin/b
```

## Asennus

1. `proxy/build/out/Release/d3d11.dll` → Fallout 4:n pelikansio (sama kansio kuin `Fallout4.exe`).
2. `plugin/b/fo4flat_probe.dll` → `Data\F4SE\Plugins\`.
3. `config/fo4stereo.ini` → pelikansio.
4. Pelin asetuksissa anti-aliasing **FXAA** (TAA aiheuttaa haamukuvia silmäsiirron kanssa).
5. Käynnistä ensin SteamVR, sitten peli F4SE:n kautta.

Poisto: poista `d3d11.dll` pelikansiosta ja plugin `Data\F4SE\Plugins`-kansiosta. Pikapoisto ilman tiedostojen
poistamista: `fo4stereo.ini`:ssä `[engine] enabled=0` ja `[vr] enabled=0`.

## Asetukset (`fo4stereo.ini`)

| Osio | Avain | Merkitys |
|---|---|---|
| `[engine]` | `enabled`, `mode=stereo\|wobble`, `ipd_units` | Silmäsiirto. 4.48 yksikköä = 64 mm. `wobble` on heiluntatesti. |
| `[vr]` | `enabled`, `eye_delay`, `swap_eyes`, `wait_poses` | SteamVR-lähetys ja silmien ajoitus |
| `[stereo]` | `enabled=0` | Vanha D3D-tason kokeilu, pidä pois päältä |

## Lisenssi ja lähteen mainitseminen

[MIT](LICENSE). Saat käyttää, muokata ja jakaa koodia vapaasti, myös kaupallisesti, kunhan lisenssiteksti ja
tekijänoikeusmerkintä säilyvät, eli **mainitset lähteenä kebabkenraalin ja tämän projektin**.

Kolmannen osapuolen osat: OpenVR-otsake (Valve, BSD-3) ja CommonLibF4 (MIT). Projekti ei sisällä pelin
tiedostoja, eikä se ole Bethesdan tekemä tai hyväksymä.

---

## English summary

**F4-Flat-2-VR** makes the regular (flat) Fallout 4 playable in a VR headset with real stereo, using an F4SE plugin
that offsets the engine's own camera per eye (found via RTTI, hooked camera-state `GetTranslation`) and a
`d3d11.dll` proxy that captures each frame per eye and submits it to SteamVR (OpenVR). Status: experimental.
License: MIT. Use it freely, just keep the notice and credit **kebabkenraali**.
