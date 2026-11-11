# Status: Fallout 4 flat -> VR (stereo + gamepad)

Päivitetty: 2026-11-11

## Tavoite

Flat Fallout 4 pelattavaksi VR-laseilla niin, että näkymä täyttää koko näkökentän ja ohjaus tapahtuu Xbox-ohjaimella. Stereo (aito kahden silmän kuva) on ensisijainen tavoite. Ohjainsilta on toissijainen.

## Mitä on tehty

### Ympäristö
- Flat Fallout 4 asennettu: `D:\SteamLibrary\steamapps\common\Fallout 4` (versio 1.11.240.0, Next-Gen).
- Flat F4SE asennettu: `f4se_1_11_240.dll`, `f4se_loader.exe`. `f4se.log` raportoi `init complete`.
- Address Library 1.11.240 on paikallaan: `F4SE\Plugins\version-1-11-240-0.bin`.
- Fallout 4 VR asennettu: `C:\Steam\steamapps\common\Fallout 4 VR` (1.2.72). FRIK (VR-mod) on käynnissä siinä, ks. `Documents\My Games\Fallout4VR\F4SE\FRIK.log`.
- SteamVR käyttää Virtual Desktopia HMD-ajurina (`ActualHMDDriver: oculus_virtualdesktop`). Quest 3 on kytketty.
- RenderDoc 1.47 asennettu (`C:\Program Files\RenderDoc`). Launch Application toimii ja kaappaukset tallentuvat.
- Visual Studio 2022 BuildTools (MSVC 14.44) toimii. Visual Studio 2026 BuildTools on poistettu.
- vcpkg asennettu polkuun `C:\vcpkg` ja alustettu. Riippuvuudet (boost, fmt, spdlog ym.) asennettu onnistuneesti.

### Stereotutkimus
- D3D11-proxy-DLL (`C:\AI_Projektit\fo4stereo`) hookkaa Presentin ja kirjaa laitteen, swapchainin ja Present-kutsut. Toimii flat-pelissä yli 17 000 kehykseen.
- **Kameradatatutkimus valmis (11.11.2026).** 4 RenderDoc-kaappausta flat-pelistä eri kamera-asennoista
  (`Temp\RenderDoc\f4se_loader_2026.11.11_00.21.35_frame{3883,4140,4348,4586}.rdc`), dumppattu
  `fo4flat_probe\dump_shell.py` (RenderDoc Python Shell; headless `--python`/`--ui-python` moodit jumittavat,
  älä yritä niitä uudelleen; raaka ReplayController ei tunne CurPipelineState:a).
  Analyysiskriptit: `analyze_cams.py`, `analyze_cams2.py`, `analyze_cams3.py`. Tulokset `camdump_N.txt`.
  - **Bufferi 2203 (752 tavua, 47 float4) = kameratilan puskuri:**
    r00–r02 kameran maailmankierto R (3×4), r04–r06 projektiokertoimet (fovX=1.1698, fovY=2.1187,
    near=15.0006; = 1/tan(fov/2), r20/r21 = tan(fov/2) → FOV ≈ 81°/51°), r08–r10 P·R,
    r12–r15 Rᵀ, r12/r13-pari viewport 2052×1133 + 1/leveys/korkeus,
    **r35/r36 kameran maailmapaikka (kahdesti)**.
  - **Bufferi 102 (336 tavua, 21 float4):** r14 = sama kameran maailmapaikka, w=4.0; r00–r03 lähitaso-
    ja FOV-vakiot; r10/r11 peilattu pari (varjot/valo); r12/r13 = viewport 2052×1133.
  - Kamerapaikan muutokset kaappausten välillä (x/y metrejä, z vakio) vahvistavat kenttien merkityksen.
  - **Miksi view-matriisissa ei ole translaatiota:** shaderi vähentää kameran maailmapaikan erikseen
    (worldPos − camPos). Siksi kamera on omassa vakiossaan.
  - **Stereo-injektiopiste:** siirrä kameran paikkavakio (buf2203 r35/r36, buf102 r14) ±IPD/2 kameran
    oikean akselin (R rivi 0) suuntaan per silmä; (worldPos − camPos) tuottaa parallaksin automaattisesti.
    Patchaus Map/Unmap-hookilla tai ennen draw-kutsuja, frame piirretään silmittäin.
- **Stereo-proxy, kokeiluversio (11.11.2026 klo 13.49):** `fo4stereo\src\proxy.cpp` siirtää kameran paikkaa
  sinimuotoisesti sivuttain (`fo4stereo.ini`: `amp_units`, `period_sec`), jotta injektiopisteen toimivuus
  näkyy ruudulla. Klo 13.16 versio kaatoi pelin heti, koska vtable-indeksit olivat väärin
  (22–25 = GSSetConstantBuffers/GSSetShader/IASetPrimitiveTopology/VSSetShaderResources). Oikeat indeksit
  varmistettu SDK:n `d3d11.h` C++-julistusjärjestyksestä: VSSetConstantBuffers 7, DrawIndexed 12, Draw 13,
  Map 14, Unmap 15, DrawIndexedInstanced 20, DrawInstanced 21, CopySubresourceRegion 46, CopyResource 47,
  UpdateSubresource 48. Koukutetaan myös ID3D11DeviceContext1–4:n vtablet erikseen (QueryInterface).
  Huom: peli piirtää instansoiduilla metodeilla (draw-imm/def = 0 logissa), eikä käytä
  UpdateSubresourcea/Copy-kutsuja lainkaan — kaikki CB-päivitykset Map/Unmapilla.
- **In-game bufferitunnistus (11.11.2026 klo 13.33–13.49):** near=15.0006-fingerprint ei osu pelin
  sisällä (f[108]=0.0 kaikissa näytteissä), eikä RenderDoc-layout (paikka r35/36) päde livenä. Posscan-
  diagnostiikka osoitti: kameran maailmapaikka (~−80692, 90978, 7974, sama tallennusalue kuin kaappauksissa)
  on 752-tavuisessa puskurissa riveillä 9/21/33/45 (toistuu ≥2 slottissa, 12 rivin välein); 576-tavuinen
  puskuri sisältää paikan riveillä 1/13/25, 288-tavuinen peilatun arvon. Päivityskanava: peli kirjoittaa
  bufferit Map/Unmapilla (upd=0, copySub=0, copyRes=0). Nykyinen patcheri: sisältöpohjainen — 752-tavuisesta
  puskurista patchataan kaikki rivit joissa |x|>10000 ja |y|>10000 ja |z|<50000, ja vain jos sama arvo
  toistuu ≥2 rivillä (erottaa kamerapaikan yksittäisistä suurista arvoista). Oikea akseli = r0 xyz jos
  yksikkövektori, muuten maailman X. Testiarvot amp_units=30, period_sec=4.
- **Korjaus 11.11.2026 klo ~14.10:** klo 13.49 proxy luki rivejä `f[r * 16]` (rivi = 4 floatia, ei 16),
  joten se luki ja kirjoitti jopa ~3 kt yli 752 tavun puskurin, eli muistikorruptiota. "posscan r9" oli oikeasti
  rivi 36 (kameran paikka). Indeksointi korjattu (`r * 4`). Lisäksi siirto tehdään nyt myös VP-matriisiin
  (rivit 8–11: `w -= dot(row.xyz, d)`), koska geometria on kamera-relatiivinen ja kulkee VP:n kautta. Pelkkä
  kameran paikan siirto vaikuttaa vain valaistukseen/sumuun. Tunnistus: rivi 6 = (0, 0, 1, −15.0). Lähdekoodin
  varmuuskopio `src\proxy.cpp.bak_r16`. Ei vielä testattu pelissä.
- **Testi 11.11.2026 iltapäivä: injektiopiste TOIMII.** Koko näkymä heiluu sivuttain (VP-siirto). Lokissa
  noin 36 000 patchia, kamerapuskuri tunnistuu. Haittana voimakas ghosting/raidoitus: TAA (temporal AA) käyttää
  edellisen kehyksen matriisia, ja muuttuva kamerasiirto rikkoo sen. `Fallout4Prefs.ini`: `sAntiAliasing`
  vaihdettu TAA -> FXAA (varmuuskopio `Fallout4Prefs.ini.bak_taa`). TAA ei sovi vuorottelevaan silmärenderiin.
- **Kamerapuskurin täydet rivit (11.11.2026, varmistettu):** 752 tavua: rivit 8–11 VP, rivit 16–19 VP⁻¹
  (rivit16–19 × rivit8–11 = I), rivit 37–40 VP:n kopio, rivit 35/36 kameran paikka. Kaikki siirretään samalla
  silmäsiirrolla (VP: `w -= dot(row.xyz,d)`, VP⁻¹: rivit 16–18 `+= d_i * rivi19`). Siirto lasketaan kerran
  kehyksessä (Present), jotta syvyys- ja värivaihe ovat samassa linjassa. 336 tavua: rivi 14 (w = 4.0) siirretään
  myös. Tunnistamatta jääneet 752 tavun puskurit ovat valojen varjokameroita (fokus ~11.4, near ~1300), eikä niitä
  siirretä. Tulos: ghosting väheni. "Osa efekteistä jäi paikalleen" -ongelma odottaa testiä tällä versiolla.
- **VP-kopiot muissa puskureissa (11.11.2026 klo 14.27 loki):** 560 B rivit 12 ja 20, 576 B rivit 12 ja 20, 224 B rivi 0
  sisältävät pääkameran VP:n kopion. 576 B rivi 4 = kameran paikan kopio. Käänteismatriisin kopioita ei löytynyt.
  Ensin tunnistus vaati tarkan vastaavuuden kehyksen VP:hen, mikä osui vain osaan latauksista (järjestys kehyksen sisällä).
  Nyt tunnistus on rakenteellinen (`IsMainVP`: rivien w = 0,0,−15.0006,0, rivi 3 xyz = yksikkövektori, rivi 2 ∥ rivi 3),
  ja kameran paikan kopio tunnistetaan ±50 yksikön toleranssilla.
- **Lasi ja varjot (11.11.2026 klo ~20.15):** testissä lasi näkyi siirtymättömänä vaaleana suorakulmiona, ja
  latausruudun malli siirtyi virheellisesti. Muutokset: (1) vain maailmakamera siirretään (kameran paikka ≠ 0, eli
  latausruudun ja UI:n kamerat ohitetaan). (2) Kamerapuskurin rivit 12–14 (Rᵀ, käänteinen näkymä) saavat
  translaation +d, jotta Rᵀ·viewPos-rekonstruktio pysyy oikeassa. (3) Yleinen tunnistus kaikille VP·W-matriiseille
  (objektikohtaiset WVP:t, esim. läpinäkyvät pinnat): rivi2.xyz = k·rivi3.xyz ja rivi2.w = k·rivi3.w + near,
  missä k ja near tulevat pääkamerasta. Korjaus c = −VP.xyz·d lisätään translaatiosarakkeeseen. Varmuuskopio
  `src\proxy.cpp.bak_scan1`. Ei vielä testattu.
- **Testi 11.11.2026 klo 20.21:** lasi pysyy edelleen paikallaan ja varjot menivät huonommiksi. Lokin mukaan
  VP·W-tunnistus osui yli miljoona kertaa (64/128/224/384/416 B puskurit), mutta lasi ei silti siirtynyt.
  Rivien 12–14 translaatiopatch on poistettu käytöstä, koska se pahensi varjoja.
  **Johtopäätös:** D3D-tason paikkaus vaatii jokaisen johdetun matriisin löytämistä (VP, VP⁻¹, kopiot,
  objektikohtaiset, varjo/valo, läpinäkyvät). Tämä ei ole kestävä tie. **Suositus:** siirrä silmäsiirto
  moottorin kameraan F4SE-pluginilla (`RE::PlayerCamera::cameraRoot`, FRIKin malli), jolloin moottori laskee
  kaikki matriisit itse ja ne pysyvät linjassa. D3D-proxyä käytetään vain kuvien kaappaamiseen ja OpenVR-lähetykseen.
- **Moottorikameran siirto (11.11.2026 klo 20.26):** F4SE-plugin `fo4flat_probe.dll` koukuttaa
  `PlayerCamera`-vtablen paikan 3 (`TESCamera::Update`). Alkuperäisen päivityksen jälkeen se siirtää
  `cameraRoot->local.translate`-arvoa kameran oikean akselin suuntaan (world.rotate, rivi 0) siniaallon mukaan, ja
  päivittää `UpdateWorldData(nullptr)`-kutsulla juuren ja lapset (FRIKin tapa). Edellinen siirto vähennetään, jos
  moottori ei ole kirjoittanut arvoa uudelleen. Asetukset ovat `fo4stereo.ini`:n `[engine]`-osiossa. D3D-proxyn
  kameran siirto on pois päältä (`[stereo] enabled=0`). Koukku asennetaan `kPostLoadGame`- tai `kNewGame`-viestissä.
  Loki kirjaa singletonin, cameraRootin maailmapaikan ja siirrot (`fo4flat_probe.log`). Ei vielä testattu.
  Plugin käännetään PowerShell-työkalulla: `Enter-VsDevShell ... -arch=x64` + `cmake --build b`.
- **Testi 11.11.2026 klo 21.10:** CommonLibF4:n `PlayerCamera::GetSingleton()` (REL::ID 1171980) osoittaa
  1.11.240:ssä väärään paikkaan (exe:n sisälle, roska-arvot). Koukku osui satunnaiseen tauluun, eikä kameran
  päivityksiä tullut. **Korjaus (21.15):** kamera haetaan RTTI:n kautta versiosta riippumatta
  (`.?AVPlayerCamera@@` → TypeDescriptor → CompleteObjectLocator → vtable → .data-osoitin oliooon). Koukku asennetaan
  vain, jos olion vtable vastaa RTTI:tä ja cameraRootin paikka on järkevä. **Huom:** CommonLibF4:n REL::ID-arvot
  ovat vanhan version arvoja, joten niihin ei voi luottaa 1.11.240:ssä. Käytä RTTI:tä tai varmista erikseen.
- **Testi 21.25:** RTTI-haku löysi oikean kameran (cameraRoot world = (−80002, 91200, 7975)), ja koukku
  asentui, mutta `TESCamera::Update` (vtable-paikka 3) ei kutsuttu kertaakaan, joten kuvaa ei siirretty.
  **21.29:** koukutetaan kamerastatejen (`PlayerCamera` +0x0E0, 13 kpl) vtablejen paikat 0x0B (Update) ja
  0x0D (GetTranslation). Siirto lisätään GetTranslationin palauttamaan paikkaan, jolloin moottori asettaa kameran
  itse. Loki kirjaa kutsumäärät (stateUpdates, getTrans, camUpdates). Asennettu, ei vielä testattu.
- **✅ ONNISTUI 11.11.2026 klo 21.36: moottorikameran silmäsiirto toimii.** Käyttäjä vahvisti: kuva heiluu
  sivuttain ja kaikki seuraa mukana (lasi, varjot, efektit). Toimiva kohta: ensimmäisen persoonan
  kameratilan (`cameraStates[0]`, vtable +25AAB08) **GetTranslation, vtable-paikka 0x0D**. Se palauttaa kameran
  paikan pelaajaan nähden (~(0, 0, 120.5) = silmäkorkeus), ja siihen lisätään `right * shift`, jossa right on
  `cameraRoot->world.rotate`:n rivi 0. GetTranslation kutsutaan noin 4 kertaa jokaista state-Updatea (0x0B) kohden.
  `TESCamera::Update` (paikka 3) ei kutsuta lainkaan. PlayerCamera löytyy RTTI:n kautta (global +30E6E58,
  vtable +25AA6D8). D3D-proxyn matriisipaikkaus on pois päältä (`[stereo] enabled=0`).
  **Seuraava vaihe:** oikea stereo. Silmät vuorottelevat (±IPD/2, noin 4,5 yksikköä = 64 mm), proxy kaappaa kuvat ja
  lähettää ne OpenVR:n kautta Quest 3:een. Silmän valinta pitää synkronoida pluginin ja proxyn välillä.
- **Stereo v1 (11.11.2026 klo ~21.45, ei vielä testattu):** plugin vaihtaa silmää jokaisessa kameratilan
  Updatessa (0x0B) ja siirtää kameraa ±ipd/2 (`[engine] mode=stereo`, `ipd_units=4.48` = 64 mm). Silmä
  kirjoitetaan jaettuun muistiin (`fo4stereo/src/stereo_shm.h`, `Localo4stereo_shm_v1`). Proxy lataa
  `openvr_api.dll`:n (SteamVR, dynaamisesti), käynnistää OpenVR:n Scene-sovelluksena, kopioi jokaisen Presentin
  takapuskurin silmän tekstuuriin ja lähettää molemmat silmät (`IVRCompositor_029`: WaitGetPoses, Submit,
  PostPresentHandoff). Asetukset `[vr]`: enabled, eye_delay (0/1), swap_eyes, wait_poses. OpenVR-otsake:
  `fo4stereo/external/openvr/openvr.h` (ValveSoftware/openvr master). Rajoitukset: ei pään seurantaa (kuva on
  päähän lukittu), kuvasuhde ja FOV eivät vastaa kypärää, vuorottelu puolittaa silmäkohtaisen kuvanopeuden,
  pelin Present on sync=2. Varmuuskopiot: `d3d11_stereo_prevr.dll.bak`, `proxy.cpp.bak_prevr`, `main.cpp.bak_wobble`.
- **Stereo v1 -testi 21.56:** OpenVR käynnistyi (HMD löytyi, `IVRCompositor_029` valmis), mutta Submit palautti
  virheen 106 = `SharedTexturesNotSupported` (ainakin 10 ensimmäistä kertaa; laskuri puuttui). Silmä vaihtui
  (seq kasvoi). Korjaus 22.00: silmätekstuurit luodaan `D3D11_RESOURCE_MISC_SHARED`-lipulla, ja loki laskee
  onnistuneet ja epäonnistuneet Submitit. Ei vielä testattu.
- **SteamVR kaatui (11.11.2026 klo 21.58):** SteamVR:n `vrclient_Fallout4.txt`: *"Failed to create sync texture.
  Ensure application was built using DXGI 1.1 or later (i.e. Call CreateDXGIFactory1)"*. Fallout 4 luo laitteen
  DXGI 1.0 -adapterilla, joten Submit antaa virheen 106, ja compositor sammuu (aiemmin 21.44 myös
  `VRInitError_Compositor_CreateDriverDirectModeResolveTextures`). SteamVR:n ajuri on `vrlink` (Steam Link).
  Korjaus 22.10: kun `[vr] enabled=1`, proxy korvaa pelin adapterin samalla fyysisellä adapterilla
  `CreateDXGIFactory1`:n kautta (LUID-vastaavuus). Ei vielä testattu.
- Aiempi havainto (vanha kaappaus, "cbPass"=cb0 12×float4, r00–r03 näkymärotaatio ilman translaatiota,
  r04–r07 projektiomatriisi, r08–r11 = P·R) vastaa samoja kenttiä — uusi analyysi täydensi sen
  kamerapaikkavakiolla ja kokonaisrakenteella.
- VR-shaderipaketti (`Shaders012_VR.fxp`) sisältää merkkijonot `EYEINDEX` ja `EYEPOS`, eli VR käyttää silmäkohtaisia vakioita.
- **Kameran sijainti suoraan moottorista (11.11.2026):** FRIK-lähdekoodi (github.com/rollingrock/Fallout-4-VR-Body,
  F4VR-CommonFramework: src/f4vr/PlayerNodes.h) näyttää mallin `RE::PlayerCamera::GetSingleton()->cameraRoot->world.translate`.
  Tämä toimii myös flat-pelissä: vendored CommonLibF4 sisältää `PlayerCamera` (singleton REL::ID 1171980),
  `TESCamera::cameraRoot` (NiPointer<NiNode> @ 0x20) ja `NiNode::world.translate`. Käytä tätä
  ground-truth-varmennukseen bufferi 2203 r35 -oletukselle (probe-pluginin laajennus).
- **FO4VR:n kameramalli (FRIK/F4VR-CommonFramework dokit):** VR:ssä kierto tehdään VR-maailman (room)
  transformiin (`g_vrWorldData`-globaali, `VRWorld_SetYaw`), ei aktoriin; HMD:n paikka yhdistyy sinne.
  VR-body-modissa pelaajan head-node seuraa kameraa ("body and camera alignment tools"). Flat-stereolle
  tärkeintä: per-silmän siirto lasketaan kameran paikasta + IPD-siirrosta kameran oikealle/vasemmalle.
- RenderDoc-kaappaukset (11.11., 4 kpl eri kamera-asennoista): `f4se_loader_2026.11.11_00.21.35_frame{3883,4140,4348,4586}.rdc`
  Temp\RenderDoc-kansiossa. `renderdoccmd convert` XML-muotoon toimii, mutta menettää datablobit;
  headless-työkalu `fo4flat_probe\dump_headless.py` (`qrenderdoc --python`) dumppaa vakiopuskuroinnit
  automatisoidusti (`camdump_N.txt`). Vanha 3,0 Gt kaappaus `frame22218.rdc` myös tallessa.

### Muuta
- Palomuuri: `codex_sandbox_offline_block_loopback_tcp` ja `…_udp` on poistettu käytöstä (hyväksytty). `codex_sandbox_offline_block_outbound` on yhä päällä.
- Probe-plugin kirjoitettu: `C:\AI_Projektit\fo4flat_probe\src\main.cpp` ja `CMakeLists.txt`.
- CommonLibF4 kopioitu: `external\commonlibf4`. Korjattu `include\F4SE\Impl\WinAPI.h` (C++23 `auto(x)` -> `std::uint32_t`), koska MSVC 14.44 ei tue sitä.
- FRIK-lähdekoodi kloonattu: `…\scratchpad\frik\src` (ei muutettu, ei asennettu flatiin).
- **Probe-plugin toimii (11.11.2026).** `fo4flat_probe.dll` latautuu, logittaa `probe loaded`, `game data ready` ja ~30 s XInput-lukemia tiedostoon `fo4flat_probe.log`. Plugin-container-infrastruktuuri vahvistettu toimivaksi.

## Probe-pluginin ratkaisut (älä tee uudelleen)

- Pluginit ladataan **`Data\F4SE\Plugins`**-kansiosta (ei pelin juuren `F4SE\Plugins`, jossa ovat vain version-binääriongelmat). Kansio luotiin 11.11., koska sitä ei ollut.
- F4SE 0.7.x vaatii `F4SEPlugin_Version`-viennin (`F4SEPluginVersionData`-struct, määritelty main.cpp:ssä) — ilman sitä "no version data" ja plugin ohitetaan.
- F4SE 0.7.x **ei kutsu `F4SEPlugin_Query`** pluginille, jolla on versiondata. Lokin alustus on siksi molemmissa Query ja Load (`SetupLog` + `std::once_flag`).
- Osoitekirjasto `version-1-11-240-0.bin` kopioitu myös `Data\F4SE\Plugins`-kansioon (CommonLibF4 vaatii sen sieltä; pelin juuren kopio on yhä F4SE:n itsensä käytössä).

## Mitä ei ole tehty

- **Aito stereo ei toimi.** Silmäkohtaista kamerasiirtoa ei ole toteutettu, eikä kahden kuvan renderöintiä.
- **Kameran sijainti: löydetty shader-datan tasolta.** Bufferi 2203 r35/r36 ja bufferi 102 r14 = kameran
  maailmapaikka (ks. Stereotutkimus). Engine-tason varmennus (`PlayerCamera::cameraRoot->world.translate`
  probe-pluginilla) tekemättä. Silmäsiirto-renderi (stereo) ei ole toteutettu.
- **Headset-testi puuttuu.** Mitään ei ole näytetty kypärässä flat-pelistä stereona eikä virtuaalinäyttönä.
- **Gamepad-silta puuttuu.** Xbox-ohjain toimii flat-pelissä sellaisenaan, mutta VR-ohjausta ei ole. Probe vahvisti XInput-lukemisen pluginista (liipaisimet LT/RT jäivät testaamatta).
- **FRIK:tä ei ole portattu flatiin.** FRIK vaatii VR-ohjaimet ja F4SE VR:n, joten portaus on iso työ.
- **Flat-peliä ei ole downgradettu.** Stereo-injektori ei sitä vaadi.

## Tunnetut ongelmat

- Kaappaus `frame4840.rdc` on poistettu. Uusi kaappaus vaatii RenderDocin Launch Applicationin käytön.
- Peli sulkeutuu suoraan, jos `Fallout4.exe` käynnistetään ilman Steamia tai launcheria. Käynnistä peli `Fallout4Launcher.exe`-kautta.
- Flat-pelin `Fallout4Prefs.ini`: ikkunan koko `2964x3172`, `bBorderless=0`. Sopii huonosti näytölle.
- VR-pelin preloader-loki (`xSE PluginPreloader.log`) raportoi import-hook-virheen. FRIK toimii silti.
- Palomuurin `codex_sandbox_offline_block_outbound` voi estää pelin verkkoyhteydet.
- Levytila: C: oli 16 Gt vapaana kaappausten jälkeen, nyt noin 54 Gt.

## Tärkeät polut

| Mitä | Polku |
|---|---|
| Flat-peli | `D:\SteamLibrary\steamapps\common\Fallout 4` |
| VR-peli | `C:\Steam\steamapps\common\Fallout 4 VR` |
| Flat-F4SE-lokit | `Documents\My Games\Fallout4\F4SE\f4se.log` |
| FRIK-loki | `Documents\My Games\Fallout4VR\F4SE\FRIK.log` |
| D3D11-proxy | `C:\AI_Projektit\fo4stereo` |
| Probe-plugin | `C:\AI_Projektit\fo4flat_probe` |
| vcpkg | `C:\vcpkg` |
| RenderDoc-kaappaukset | `%USERPROFILE%\AppData\Local\Temp\RenderDoc` |
| Analyysi | `…\scratchpad\renderdoc\cbpass_dump.txt` |
