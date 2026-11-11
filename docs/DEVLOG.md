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
- **DXGI 1.1 -korjaus toimii (11.11.2026 klo 22.19):** peli pyörii kypärässä, `submitOk` kasvaa, eikä virheitä tule.
  Käyttäjän havainto: molemmat silmät näyttävät saman kuvan. **Syy:** ensimmäisen persoonan kameratilan
  alkuperäinen GetTranslation palauttaa paikan, jossa edellisen kehyksen siirto on jo mukana
  (wobble: out ≈ shift_n + shift_{n-1}; stereo: shift −2.24 → out 0). Vuorottelevat ±IPD/2 kumoavat toisensa.
- **Stereo v2 (22.25, asennettu, ei testattu):**
  - Jaettu muisti v2 (`Localo4stereo_shm_v2`): eye, seq, presents, flatMode, HMD-asento seqlockilla.
  - **Proxy omistaa silmän:** kehyksen silmä = (Present-määrä ennen tätä − eye_delay) & 1. Plugin lukee saman
    laskurin state-Updatessa.
  - **Kompensointi:** GetTranslation palauttaa `orig − prevApplied + curApplied` (`[engine] compensate=1`).
  - **2D-ruutu:** jos kameran päivityksiä (seq) ei tule 20 kehykseen (intro, päävalikko, lataus), proxy näyttää
    kuvan IVROverlay-ruudulla (3 m leveä, 2 m edessä istuma-avaruudessa) ja lähettää silmille mustaa.
  - **Pään seuranta:** proxy kirjoittaa WaitGetPoses-asennon (HMD, istuma-avaruus). Plugin koukuttaa kameratilan
    GetRotationin (0x0C) ja yhdistää pään kierron (VR → peli: q = (w, x, −z, y), `head_rotation` 1/2,
    `head_rot_compensate`). Pään paikka lisätään siirtoon (`head_position`, `units_per_meter=70`).
  - Varmuuskopiot: `d3d11_vr_v1.dll.bak`, `Data\F4SEo4flat_probe_stereo1.dll.bak`, `fo4stereo.ini.bak_v1`,
    `proxy.cpp.bak_vr1`, `main.cpp.bak_stereo1`.
- **Stereo v2 -testi 22.28:** VR ei käynnistynyt: `VRInitError_Init_HmdNotFound` (108), koska kypärä ei ollut
  yhteydessä SteamVR:ään pelin käynnistyessä. Siksi 2D-ruutua ja pään seurantaa ei testattu (`pose=false`).
  Silmä vuorottelee pluginissa (eye 0/1, cur ±ipd/2). 600 kehyksen välein otetut näytteet osuvat aina samaan
  pariteettiin, joten erottelua ei voi vielä varmistaa. Mallin mukaan (palaute g ≈ 1, `out = orig − prev + cur`)
  silmät vuorottelevat arvojen 0 ja −2·(ipd/2) välillä, eli erottelu = IPD. Lisätty 22.31: proxy yrittää OpenVR:n
  käynnistystä 10 s välein (VR_ShutdownInternal epäonnistumisen jälkeen), ja plugin kirjaa jokaisen
  GetTranslation-kutsun väliltä #4000–#4047 varmistusta varten.
- **Stereo v2 -testi 22.36 (käyttäjä):** valikot, intro ja latausruutu näkyvät 2D-ruudulla kuten pitää ✅.
  VR-tilassa molemmat silmät näyttävät edelleen saman kuvan, eikä pään kääntäminen tee juuri mitään.
  Loki: HMD-asento on kelvollinen (esim. q = 0.901/0.424/…), mutta GetRotationin orig ei sisällä edellisen
  kehyksen pään kiertoa. Siksi `head_rot_compensate=1` kumosi pään kierron (out ≈ orig). Translaatiossa orig ≈
  edellisen kehyksen out (feedback), mutta ei tarkasti: out liukuu kohti oikeaa paikkaa, joten
  `orig − prev + cur` -kompensointi ei pidä silmiä erillään.
  **Korjaus 22.39:** `compensate=3` (oletus): jokaisen state-Updaten alussa cameraRoot palautetaan siirtämättömään
  kohtaan (local ja world −= edellinen siirto), ja GetTranslation lisää pelkän `cur`:n. `head_rot_compensate=0`
  (oletus). Loki kirjaa kutsut #400–#447 ja rootLocal-arvon. Ei vielä testattu.
- **Testi 22.42 (compensate=3):** **pään kääntäminen toimii ✅** (käyttäjä vahvisti; `head_rot_compensate=0`).
  Silmät: `compensate=3` ei auta. Kun cameraRoot palautetaan, orig_n ≈ T + cur_{n−1} silti (palaute tulee muualta,
  todennäköisesti 1. persoonan luurangosta), ja out pysyy samana kummallakin silmällä. Edellisen ajon jäljen
  (#4–#8, #600) mukaan `compensate=1` antaa oikein out = T + cur, ja peräkkäisten kehysten ero ≈ IPD.
  Present tapahtuu kesken seuraavan kehyksen GetTranslation-kutsuja (lokissa presents kasvaa kehyksen sisällä),
  eli kuva esitetään kehyksen myöhässä, ja silmät menivät ristiin. **Asetukset 22.45:** `compensate=1`,
  `eye_delay=1`, `ipd_units=9` (kaksinkertainen testiä varten, palautetaan 4.48:aan). Varmuuskopio `fo4stereo.ini.bak_v3`.
  Seuraava iso aihe (käyttäjä): miten kuva renderöidään laseille (FOV, kuvasuhde, projektio).
- **Fokusongelma (käyttäjä 22.50):** peli ei latautunut eikä näyttänyt kuvaa kypärässä, ennen kuin sen ikkunaa
  klikattiin hiirellä. Fallout 4 pysähtyy ilman fokusta, ja SteamVR vie fokuksen. Korjaukset:
  `Documents\My Games\Fallout4\Fallout4Custom.ini` → `[General] bAlwaysActive=1` (varmuuskopio `.bak_focus`,
  tiedosto pidetty vain luku -tilassa). Proxy antaa pelin ikkunalle fokuksen kerran VR:n käynnistyttyä
  (`SetForegroundWindow`, loki `vr: focus game window`). Ei vielä testattu.
- **Kypärän FOV ja kuvasuhde, vaihe 1 (23.05, asennettu, ei testattu):** käyttäjä valitsi vaihtoehdon "pelin
  resoluutio ja FOV vastaamaan kypärää". Proxy lukee VR:n käynnistyessä IVRSystemistä
  `GetRecommendedRenderTargetSize`- ja `GetProjectionRaw`-arvot molemmille silmille, kirjaa ne lokiin ja kirjoittaa
  suosituksen `fo4stereo_hmd.txt`-tiedostoon (ikkunan koko ja fDefaultWorldFOV symmetrisestä kattavasta FOV:sta).
  Lisäksi Submit käyttää silmäkohtaisia `VRTextureBounds_t`-rajoja, jotka lasketaan pelin todellisesta projektiosta
  (kamerapuskurin rivit 4/5: tan = 1/x, 1/y). Näin kuva kohdistuu kulmallisesti oikein millä tahansa pelin FOV:lla.
  Asetus `[vr] fov_bounds=1`. **Vaihe 2:** kun `fo4stereo_hmd.txt` on olemassa, aseta `Fallout4Prefs.ini`:n
  iSize W/H ja `Fallout4Custom.ini`:n `[Display] fDefaultWorldFOV`/`fDefault1stPersonFOV` suosituksen mukaan.
  Mittaus: FOV-asetus on vaakasuuntainen (80 → hfov 81° kuvasuhteella 1.81).
- **Kypärän mittaus (23.10):** Quest 3 (Steam Link) silmäkohtainen suositus 2312×2524. Tangentit
  vasen l/r/t/b = −1.376/0.839/−1.428/0.966, oikea −0.839/1.376/−1.428/0.966. Kattava symmetrinen FOV 108° × 110°,
  kuvasuhde 0.964. Pelin FOV-määritelmä selvitettiin: vaakasuuntainen FOV 16:9:llä, ja todellinen hfov skaalautuu
  Hor+ (asetus 80 → 81.06° kuvasuhteella 1.811, täsmälleen). **Vaihe 2 asetettu:** `Fallout4Prefs.ini` iSize
  1946×2019 (0.8 × suositus, kuvasuhde 0.964), `Fallout4Custom.ini [Display]` fDefaultWorldFOV ja
  fDefault1stPersonFOV = 137 (= 108° vaaka kuvasuhteella 0.964). Varmuuskopiot `*.bak_hmdfov`, tiedostot
  pidetty vain luku -tilassa. Tarkistus: seuraavan ajon loki `vr: eye N bounds ... game tan x≈1.376 y≈1.428`.
  Jos peli rajoittaa FOV:n, tan on pienempi, ja reunat venyvät (bounds rajautuvat).
  SetForegroundWindow palautti 0 (fokuksen anto epäonnistui). bAlwaysActive=1 hoitaa tilanteen, jos se toimii.
- **Testi 23.03:** ikkunan korkeus rajautui näytön mukaan 1133 pikseliin (iSize H=2019 → backbuffer 1946×1133),
  joten kuvasuhde oli 1.72. Pelin tan x=2.453 (≈136°) ja y=1.428 (110° ✓). Bounds rajasi vaakasuunnassa oikein.
  Korjaus: iSize 1092×1133 (kuvasuhde 0.964 mahtuu näytölle; FOV 137 → 108° vaaka). Silmät: `compensate=1`
  antoi molemmille silmille saman out ≈ (0, 0, 120.48). Tällä kertaa orig = T + prev − cur, eli moottori kumosi
  siirron täysin. **Uusi tapa `compensate=4` (23.05):** silmä- ja pääsiirto asetetaan cameraRootin lapsisolmun
  (renderöintikamera, nimi lokissa) local.translate-arvoon kameran paikallisissa akseleissa. Kameratila ei lue
  lasta, joten palautetta ei tule. GetTranslationia ei muuteta. Ei vielä testattu. Varmuuskopiot `main.cpp.bak_v3`,
  `Data\F4SEo4flat_probe_v3.dll.bak`, `fo4stereo.ini.bak_v4`.
- **Käyttäjän havainnot (23.03-ajo, compensate=1, ipd 9):** "hieman kaksoiskuvaa esim. kun NPC on ihan vieressä;
  tähtääminen sekoittaa näkymän; kameran liikkeissä lagia; Pip-Boy pitäisi skaalata paremmin tai näyttää muuten kuin
  VR:ssä." Kaksoiskuva lähellä tarkoittaa, että silmät erosivat jonkin verran. Silmäväli oli kaksinkertainen (9).
  Tehty 23.10: `ipd_units=4.48` (todellinen). Proxy: `[vr] no_vsync=1`, eli VR:n aikana Present kutsutaan sync 0:lla,
  koska peli käytti sync=2 (30 fps 60 Hz:llä) ja WaitGetPoses tahdistaa kehykset kypärän mukaan. Avoinna:
  tähtäys (zoom kaventaa pelin FOV:n, ja bounds venyttää kuvan; ratkaisuna ADS-zoomin ohitus VR:ssä tai
  FOV:n lukitus) ja Pip-Boy 2D-ruudulle (tarvitsee valikkotunnistuksen, esim. RTTI:llä löydetty UI +
  MenuOpenCloseEvent). Fallout 4:n fysiikka on sidottu kuvanopeuteen, joten yli 60 fps voi aiheuttaa ongelmia
  (Quest 72 Hz suositeltava).
- **Automaattinen kypäräkonfigurointi (23.20, asennettu, ei testattu):** `[vr] auto_configure=1`. Kun VR
  käynnistyy, proxy laskee mitatusta näkökentästä ikkunan (kypärän kuvasuhde, korkeus = min(suositus,
  työalueen korkeus − ikkunan reunat)) ja FOV:n kaavalla `2·atan(tan(hfov/2)·(16/9)/kuvasuhde)`. Arvot kirjoitetaan
  tiedostoihin `Documents\My Games\Fallout4\Fallout4Prefs.ini [Display] iSize W/H` ja
  `Fallout4Custom.ini [Display] fDefaultWorldFOV/fDefault1stPersonFOV` (Documents-polku SHGetKnownFolderPath-
  funktiolla). Kirjoitus tehdään vain muuttuneille arvoille. Vain luku -tila säilytetään, ja kertaluonteinen
  varmuuskopio tehdään nimellä `*.bak_fo4stereo`. Muutokset tulevat voimaan seuraavalla käynnistyksellä
  (loki: `vr: auto-config ... UPDATED, restart the game`). Toimii mille tahansa SteamVR-laseille.
- **Testi 23.14 (compensate=4 + auto-config):** käyttäjä: "much better". Loki: renderöintikamera `WorldRoot Camera`
  (cameraRootin lapsi 0) saa silmäkohtaisen siirron (x −0.99 / +3.48, ero = IPD) ✅. Pelin tan 1.376/1.428 vastaa
  kypärää täsmälleen ✅. Auto-config kirjoitti ikkunan 1026×1065 (työalueen raja). Avoinna käyttäjältä: Pip-Boy
  liian suuri, tähtäyksessä kaksoiskuva ja VATS ei toimi.
  **Korjaukset 23.18 (asennettu, ei testattu):** proxy tunnistaa zoomin pelin projektiosta
  (suhde = pelin tan / kypärän tan: < aim_ratio 0.95 → tähtäys, < pipboy_ratio 0.5 → Pip-Boy) ja kirjoittaa sen
  jaettuun muistiin (`zoom`). Plugin kirjoittaa aktiivisen kameratilan (`camState`). Tähtäyksessä silmäsiirto
  × `aim_ipd_scale` (0 = mono). Pip-Boy (zoom 2) ja muut kameratilat kuin 1. persoona (VATS 2, dialogi,
  3. persoona…) näytetään 2D-ruudulla (`pipboy_2d`, `other_cams_2d`). Stereo ja pään seuranta vain kameratilassa 0.
  Loki: `vr: zoom=N (game tan x=…, ratio …) camState=N` tilan vaihtuessa, joten kynnysarvot voi kalibroida.
- **Testi 23.22:** tähtäys toimii (mono) ✅, Pip-Boy 2D:nä ✅, VATS 2D:nä ✅. Kolmas persoona näkyi 2D:nä, vaikka sen
  pitäisi olla VR:ssä. Loki: kolmas persoona raportoituu kameratilana 7, koska tilat 7 ja 8 jakavat vtablen ja
  plugin rekisteröi ensimmäisen. Mitatut zoomit: tähtäys 0.944–0.949, Pip-Boy 0.445–0.454 (välissä 0.53 ja 0.82
  siirtymissä). **Korjaus 23.26:** `[vr] vr_cam_states=0,7` (proxy ja plugin) määrää VR:ssä näytettävät kameratilat.
  Muut näytetään 2D:nä. Stereo ja pään seuranta koskevat näitä tiloja. Ei vielä testattu.
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
