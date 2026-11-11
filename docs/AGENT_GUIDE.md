# Ohjeistus AI-agentille: Fallout 4 flat -> VR

Päivitetty: 2026-11-11 klo 22.30. Lue tämä ensin ja sitten `STATUS.md`, jossa on yksityiskohtainen historia.

## Tavoite ja tilanne yhdellä silmäyksellä

Flat Fallout 4 (1.11.240, Next-Gen) pelattavaksi Quest 3:lla aidolla stereokuvalla ja Xbox-ohjaimella.

| Osa | Tila |
|---|---|
| Silmäsiirto moottorin kameraan (F4SE-plugin) | ✅ Toimii. Heiluntatesti vahvisti: lasi, varjot ja efektit seuraavat. |
| Silmien vuorottelu ±IPD/2 ja jaettu muisti | ✅ Rakennettu. Silmä vaihtuu (seq kasvaa lokissa). |
| Kuvien lähetys SteamVR:lle (proxy) | ✅ Toimii DXGI 1.1 -korjauksen jälkeen (submitOk kasvaa). |
| Eri kuva kummallekin silmälle | ⚠️ Kameratila palauttaa edellisen kehyksen siirron (orig ≈ T + cur_prev). `compensate=1` korjaa laskennallisesti, mutta kuva esitetään kehyksen myöhässä, joten tarvitaan `eye_delay=1`. **Ei vielä vahvistettu kypärässä.** `compensate=3` ei toimi. |
| Valikot, intro ja lataus 2D-ruudulla | ✅ Toimii (käyttäjä vahvisti): IVROverlay-ruutu, kun kameran päivityksiä ei tule. |
| Pään seuranta (HMD-asento → kamera) | ✅ Kierto toimii (käyttäjä vahvisti 22.42): GetRotation 0x0C, `head_rotation=1`, `head_rot_compensate=0`. Paikka (6DoF) lisätään siirtoon, ei erikseen varmistettu. |
| Kypärän FOV ja kuvasuhde | ⚠️ Proxy mittaa kypärän (fo4stereo_hmd.txt), rajaa silmäkohtaisesti (`fov_bounds`) ja kirjoittaa ikkunan koon ja FOV:n peliin automaattisesti (`auto_configure=1`). Ikkunan korkeus rajautuu näytön korkeuteen. **Ei vielä vahvistettu kypärässä.** |
| Gamepad | Toimii flat-pelissä sellaisenaan. Plugin lukee XInputia (vain loki). |

## Arkkitehtuuri

```
Fallout4.exe
 ├─ Data\F4SE\Plugins\fo4flat_probe.dll   (F4SE-plugin, C:\AI_Projektit\fo4flat_probe)
 │    - löytää PlayerCameran RTTI:llä (.?AVPlayerCamera@@)
 │    - koukuttaa kameratilojen vtablet: 0x0B Update (vaihtaa silmää), 0x0D GetTranslation (lisää siirron)
 │    - kirjoittaa silmän jaettuun muistiin
 ├─ d3d11.dll (pelikansion juuressa)       (proxy, C:\AI_Projektit\fo4stereo)
 │    - välittää kutsut järjestelmän d3d11.dll:lle
 │    - [vr] enabled=1: korvaa pelin adapterin DXGI 1.1 -adapterilla (sama GPU, LUID)
 │    - Present-koukku: kopioi takapuskurin silmän tekstuuriin ja lähettää molemmat silmät OpenVR:lle
 └─ jaettu muisti "Local\fo4stereo_shm_v1" (fo4stereo\src\stereo_shm.h): eye, seq, presents
```

Asetukset ovat tiedostossa `D:\SteamLibrary\steamapps\common\Fallout 4\fo4stereo.ini`:

- `[stereo] enabled=0`: vanha D3D-matriisipaikkaus, pidä pois päältä.
- `[engine] enabled, mode=stereo|wobble, ipd_units=4.48, amp_units, period_sec`: wobble on heiluntatesti.
- `[engine] compensate, head_rotation (0/1/2), head_rot_compensate, head_position, units_per_meter`.
- `[vr] enabled, eye_delay (0/1), swap_eyes (0/1), wait_poses (0/1), flat_after_frames, screen_width_m, screen_distance_m`.

Pikapoisto: `[engine] enabled=0` ja `[vr] enabled=0` palauttavat tavallisen pelin.

## Käyttäjä

- Suomenkielinen, vastaa suomeksi.
- Agentti ei näe näyttöä eikä kypärää. Käyttäjä käynnistää pelin, testaa ja raportoi. Pyydä yksi konkreettinen
  toimenpide kerrallaan ja lue sen jälkeen lokit itse.
- **Peli lukitsee DLL:t.** Asenna uusi `d3d11.dll` tai plugin vain, kun `tasklist | grep Fallout4.exe` on tyhjä.
  Pyydä käyttäjää sulkemaan peli. Älä sulje sitä itse ilman lupaa.
- Hyväksytty aiemmin: VS 2022 BuildTools, vcpkg `C:\vcpkg`, RenderDoc, palomuurin loopback-sääntöjen poisto,
  OpenVR-otsakkeen lataus. Uusia lupia ei voi päätellä vanhoista. Järjestelmätason tai hankalasti peruttavat
  muutokset vaativat erillisen luvan.

## Käännös

**Proxy (`C:\AI_Projektit\fo4stereo`)**, toimii Git Bashista:
```bash
cd /c/AI_Projektit/fo4stereo && cmake --build build --config Release
# tulos: build/out/Release/d3d11.dll
```

**Plugin (`C:\AI_Projektit\fo4flat_probe`)**, vaatii 64-bit VS 2022 -ympäristön. Toimii PowerShell-työkalulla:
```powershell
Import-Module "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
Set-Location C:\AI_Projektit\fo4flat_probe; cmake --build b
# tulos: b\fo4flat_probe.dll   ("vswhere.exe not recognized" -rivi on harmiton)
```
Täysi uudelleenkonfigurointi: `cmake -S . -B b -G Ninja -DCMAKE_BUILD_TYPE=Release`.

Huomioitavaa pluginin käännöksessä:
- Vain VS 2022 (MSVC 14.44). VS 2026 on poistettu. Älä aseta `VCPKG_VISUAL_STUDIO_PATH`-muuttujaa.
  Generaattori on Ninja, koska `Visual Studio 17 2022` ei löydä instanssia.
- vcpkg on `C:\vcpkg`. Pitkät polut aiheuttavat virheen `Cannot open compiler generated file: ''`.
- C++23, `NOMINMAX` ja `WIN32_LEAN_AND_MEAN`. `external\commonlibf4\...\F4SE\Impl\WinAPI.h` on muokattu
  (`auto(x)` → `std::uint32_t`), älä palauta. Vain CommonLibF4-kirjasto lisätään, ei sen työkaluja.
- CommonLibF4-otsakkeet ennen `<windows.h>`:ta (`main.cpp`:n alku).

## Asennus (peli suljettuna)

```bash
G="/d/SteamLibrary/steamapps/common/Fallout 4"
cp /c/AI_Projektit/fo4stereo/build/out/Release/d3d11.dll "$G/d3d11.dll"
cp /c/AI_Projektit/fo4flat_probe/b/fo4flat_probe.dll "$G/Data/F4SE/Plugins/fo4flat_probe.dll"
```
Ota varmuuskopio ennen korvaamista (`*.bak`-nimellä pelikansioon, ei `Plugins`-kansioon). Tarkista md5sum.

## Lokit

| Loki | Polku |
|---|---|
| Proxy | `D:\SteamLibrary\steamapps\common\Fallout 4\fo4stereo_proxy.log` (`vr:`-rivit, `dxgi11:`, Present-laskurit) |
| Plugin | `Documents\My Games\Fallout4\F4SE\fo4flat_probe.log` (`rtti:`, `state N ... hooked`, `getTrans #`) |
| F4SE | `Documents\My Games\Fallout4\F4SE\f4se.log` |
| SteamVR, peli asiakkaana | `C:\Steam\logs\vrclient_Fallout4.txt` (tärkein VR-vianetsintään) |
| SteamVR compositor/server | `C:\Steam\logs\vrcompositor.txt`, `vrserver.txt` |

## Testausohje käyttäjälle (nykyinen vaihe)

1. Käynnistä SteamVR ja yhdistä Quest 3 (ajuri on Steam Link, `vrlink`). Odota, että kypärässä näkyy SteamVR:n koti.
2. Käynnistä peli F4SE:n kautta ja lataa tallennus.
3. Raportoi, mitä kypärässä näkyy. Lue sen jälkeen lokit: proxy (`dxgi11:`, `vr: frame ... submitOk/submitErr`)
   ja `vrclient_Fallout4.txt`.

## Tärkeät opit (älä toista virheitä)

- **Älä paikkaa matriiseja D3D-tasolla.** Kokeiltiin: VP, VP⁻¹, kopiot, objektien WVP:t. Lasi ja varjot eivät
  silti seuranneet. Silmäsiirto tehdään moottorin kameraan.
- **CommonLibF4:n `REL::ID`-arvot ovat vanhan version arvoja** eivätkä toimi versiossa 1.11.240
  (PlayerCamera-ID osoitti exe:n sisälle). Hae oliot RTTI:llä (`FindObjectByRtti`) ja tarkista arvojen järkevyys.
- `TESCamera::Update` (vtable-paikka 3) ei kutsuta. Toimiva paikka on **kameratilan `GetTranslation` (0x0D)**.
  `Update` (0x0B) kutsutaan kerran kehyksessä, `GetTranslation` noin 4 kertaa.
- **D3D11-vtable-indeksit** luetaan SDK:n `um\d3d11.h`-tiedostosta, ei arvata. Oikeat: Map 14, Unmap 15,
  DrawIndexed 12, Draw 13, UpdateSubresource 48. Väärät indeksit (22–25) kaatoivat pelin.
- **Vakiopuskurin rivi = 4 floatia** (`f[r*4]`). Aiempi koodi käytti `f[r*16]`, ja se aiheutti muistikorruption.
- **OpenVR vaatii DXGI 1.1:n.** Fallout 4 käyttää DXGI 1.0:aa → `SharedTexturesNotSupported` (106) ja compositor
  kaatuu. Proxy korvaa adapterin (`GetDxgi11Adapter`).
- **Fokus:** Fallout 4 pysähtyy ilman ikkunan fokusta (musta kypärä, kunnes ikkunaa klikataan). `Fallout4Custom.ini` `[General] bAlwaysActive=1` ja proxyn `SetForegroundWindow`.
- **TAA rikkoo silmäsiirron.** `Fallout4Prefs.ini`: `sAntiAliasing=FXAA` (varmuuskopio `.bak_taa`).
- **Python-heredocit ja kenoviivat:** C-koodin `'\\'`- ja `"C:\\..."`-merkkijonot ovat menneet rikki kahdesti,
  kun koodia lisättiin Pythonilla (`'\'` → käännösvirhe "newline in constant"). Tarkista kenoviivat lisäyksen
  jälkeen tai tee ne Edit-työkalulla.
- **Rinnakkaiset istunnot ovat muokanneet samoja tiedostoja** (`proxy.cpp`, `main.cpp`, `STATUS.md`). Lue
  tiedosto aina ennen muokkausta, äläkä oleta sen sisältöä muistista.
- Peli: käynnistä F4SE:n tai launcherin kautta. Suoraan käynnistetty `Fallout4.exe` sulkeutuu heti.
  RenderDoc: käytä UI:n Launch Applicationia, ei `renderdoccmd inject`ia. Steamin F12 on kuvakaappausnäppäin.

## Kielletyt tai luvanvaraiset toimet

- Älä sulje käyttäjän peliä, poista kaappauksia (gigatavuja) tai muuta palomuuria ilman lupaa.
  `codex_sandbox_offline_block_outbound` on yhä päällä.
- Älä kierrä automaattisen oikeustarkistuksen estoja. Kysy käyttäjältä.
- Älä lataa modeja. Nexus vaatii kirjautumisen, joten käyttäjä lataa itse.

## Seuraavat askeleet (tärkeysjärjestyksessä)

1. **Testaa stereo v2.** Tarkista lokista `getTrans`-rivit: `orig` ja `out` kummallekin silmälle (out:n pitää
   vaihdella ±ipd/2 oikean akselin suuntaan) ja `vr: mode -> 2D screen / 3D stereo`. Jos syvyys on väärin päin:
   `swap_eyes=1`. Jos silmät eivät täsmää: `eye_delay=1`.
2. **Pään seuranta:** v2 on rakennettu. Tarkista `getRot`-rivit: jos orig sisältää edellisen kehyksen pään kierron,
   `head_rot_compensate=1` on oikein. Jos kierto ei juuri liiku, kokeile 0. Jos kierto menee väärään suuntaan,
   kokeile `head_rotation=2`. Paikka käyttää kameran akseleita, joissa on mukana pään yaw. Oikeampi on vartalon yaw.
3. **FOV ja kuvasuhde:** lue `GetProjectionRaw`, aseta pelin resoluutio ja FOV (`fDefaultWorldFOV`) vastaamaan
   kypärää tai rajaa kuva `VRTextureBounds_t`:lla.
4. **Kuvanopeus:** peli käyttää Present(sync=2). Harkitse `iPresentInterval`-asetusta. Fallout 4:n fysiikka on
   sidottu kuvanopeuteen, joten älä nosta yli 60 fps ilman rajoitinta.
5. Myöhemmin: pelkän kehyksen vuorottelun sijaan kaksi renderöintiä kehyksessä (vaatii moottorin
   renderöintikutsun toistamisen) ja FRIK-tyyppiset VR-ominaisuudet.
