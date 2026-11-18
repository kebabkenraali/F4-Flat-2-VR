# Uusi suunnitelma – F4-Flat-2-VR (laadittu 16.11.2026)

Perustuu koodin, git-tilan, pelikansion lokien ja kaikkien ohjetiedostojen läpikäyntiin
(AGENTS.md, fo4flat_probe\AGENT_GUIDE.md + STATUS.md, repon README, docs\TODO.md, docs\DEVLOG.md).

## 1. Tavoite

Flat Fallout 4 (1.11.240 NG) pelattavaksi VR-laseilla (Quest 3 / SteamVR): F4SE-plugin siirtää moottorin
render-kameraa silmäkohtaisesti ja soveltaa pään asentoa; d3d11.dll-proxy lähettää kuvat OpenVR:ään ja näyttää
valikot 2D-ruudulla. Ohjaus Xbox-ohjaimella. Motion-ohjaimet myöhemmin.

## 2. Mikä on osoittautunut toimivaksi (testattu pelissä)

| Asia | Todiste / huomio |
|---|---|
| Moottorikameran silmäsiirto (`compensate=4`, lapsisolmu `WorldRoot Camera`) | Käyttäjä 11.–12.11. "much better"; D3D-matriisipaikkaus hylätty lopullisesti |
| Pään kierto (`gameQ=(w,x,-z,y)`, `head_rot_compensate=0`) ja 6DoF-paikka | Vahvistettu 15.11. (pahoinvointi poistui) |
| Valikot / intro / lataus 2D-ruudulla, `bAlwaysActive=1` | Toimii |
| Kypärän FOV + resoluution auto-konfigurointi, per-silmä bounds | Pelin tan = kypärän tan (1.376/1.428) |
| FSR1-upscale (ei-temporaalinen) | Toimii (testattu 15.11. DEVLOGin mukaan) |
| Pip-Boy 2D-ruudulla (projisoitu RT), VATS 2D:nä | Testattu 12.11. ✅ |
| Runtime IPD + world scale | Toteutettu 15.11. |

## 3. Mikä EI ole toiminut / on kesken

| Ongelma | Tila |
|---|---|
| **Tuplakuva tähtäyksessä (ADS)** | Ratkaisematon. Useita arvauksia (silmän valinta, mono-bounds, jaettu tekstuuri, aim_ipd_scale, dominantti silmä) – mikään ei ole todistetusti korjannut. Viimeisin hypoteesi: molemmille silmille sama D3D-tekstuuri → korjattu erillisiin tekstuureihin, **ei testattu**. |
| **Peli kaatuu käynnistyksessä (viimeisin ajo 15.11. klo 21.28)** | Katso kohta 4. Tämä estää kaiken muun testauksen. |
| RenderDoc-kaappaus | Pre-launch kaataa pelin (gameoverlayrenderer64 + RenderDoc/DXGI pinossa). Ei yhtään .rdc-tiedostoa. |
| Aim-tunnistus pluginissa | `fovAdjustCurrent` luki 0 (väärä offset 1.11.240:ssä) → koodattiin korvaava lukeminen, tulos varmistamatta |
| Kamera "alas-vasemmalle" | Recenter-avain (Delete) + yaw/pitch-offsetit rakennettu, testaamatta |
| Alternate-frame stereo (puolet kuvanopeudesta) | Rakenteellinen rajoite; per-silmä 30 Hz |
| 3. persoona, kiikarit | `vr_cam_states=0` (3. persoona 2D-ruudulle); kiikarit testaamatta |
| Motion-ohjaimet | Ei aloitettu (FRIK on GPL → vain ideat) |

## 4. Kriittinen löydös: peli kaatuu heti (korjattava ensin)

Viimeisimmän session (15.11. 21.28) jäljet:
- `fo4stereo_proxy.log` on 3510 riviä, ja vain sama käynnistysblokki toistuu **585 kertaa** (`vr config…` +
  `dxgi11: using adapter 0…`). Lokissa ei ole yhtään `vr: …` -riviä (OpenVR-init, zoom, bounds). Proxy siis
  luo/alustaa laitteen kerta toisensa jälkeen eikä etene Present-vaiheeseen.
- Plugin-lokissa on yksi rivi (`probe loaded`), ei `game data ready` / `hooked` -rivejä → peli kaatui ennen kuin
  pelidata ehti latautua.
- Crash dump -tiedostoja 15.11. kymmenen kappaletta (klo 15.33–21.28), viimeisin `Fallout4.exe.23228.dmp`
  21.28.38 (sama hetki kuin lokin loppu).
- Asennettu `d3d11.dll` ja `fo4flat_probe.dll` ovat täsmälleen samat kuin repon buildit (SHA-256 täsmää), eli
  kaatuva koodi on nykyinen repon työkopio (muokkaamaton `git diff` ~1000 riviä, ei commitoitu).
- Epäillyt (ei vielä todennettu): (a) `CreateDevice`-uudelleenyritysten silmukka proxyn adapterinvaihdossa
  (DXGI 1.1 -factory) – esim. RenderDoc/Steam-overlay-hookin kanssa tai epäonnistuvan laitteen luonnin jälkeen;
  (b) RenderDoc-API:n lataus/`capture_renderdoc`-koodi proxyssa (lisätty 15.11.); (c) Steam-overlay-DLL.
  Peli käynnistyi viimeksi onnistuneesti aiemmin samana päivänä (ADS-lokit), joten regressio on syntynyt
  15.11. iltapäivän/illan muutoksista (distinct eye submit, capture-tuki, RenderDoc-API).

**Dump-analyysi tehty 16.11. (cdb, `Fallout4.exe.23228.dmp`) – syy löytyi:**
- Poikkeus on pinon ylivuoto (`AV.InvalidStackAccess`, KERNELBASE!MultiByteToWideChar), ei pelikoodin virhe.
- Pino on ääretön rekursio: `d3d11!D3D11CreateDeviceAndSwapChain` (meidän proxy) → `renderdoc!…` →
  `gameoverlayrenderer64!…` (Steam-overlay) → takaisin `d3d11!D3D11CreateDeviceAndSwapChain` → … kymmeniä
  kierroksia, kunnes pino loppuu. Jokainen kierros kirjasi lokiin `dxgi11: using adapter…` (siksi 584 toistoa,
  eikä yhtään `D3D11CreateDevice hr=` -riviä: kutsu ei koskaan palannut).
- Syy: peli oli käynnistetty RenderDocin alaisena (`capture_renderdoc.ps1`). RenderDoc ja Steam-overlay hookkaavat
  kumpikin d3d11/dxgi-kutsuja, ja koska proxymme on itse nimeltään `d3d11.dll`, hookkien "alkuperäinen funktio"
  osoittaa takaisin proxyyn → rekursio. Proxyssa ei ole uudelleentulosuojaa
  (`proxy.cpp` `D3D11CreateDeviceAndSwapChain`/`D3D11CreateDevice`, rivit ~2594–2651).
- Kaikilla kymmenellä dumpilla on sama ylin pino (gameoverlayrenderer64 / MultiByteToWideChar), eli kaikki
  15.11. kaatumiset ovat todennäköisesti samaa RenderDoc-yritystä (täysi rekursiopino varmistettu vain dumpista
  23228). **Normaali käynnistys ilman RenderDocia ei ole kaatunut näissä dumpeissa** – regressiota pelin
  normaalissa ajossa ei ole osoitettu.

**Päätelmä:** peli ei ole "rikki"; RenderDoc-käynnistystapa on. Seuraavaksi: (1) käyttäjä käynnistää pelin
normaalisti F4SE:llä (ei `capture_renderdoc.ps1`) ja varmistaa, että lokiin tulee `vr: …`-rivejä;
(2) proxyyn lisätään uudelleentulosuoja (thread-local-laskuri: sisäkkäisellä kutsulla kutsutaan suoraan
System32-d3d11:n vientiä eikä jatketa hookkausta), jotta virhetilanne ei enää kaada peliä pinon ylivuotoon;
(3) RenderDoc-API-koodi pidetään oletuksena pois päältä, kunnes ADS-diagnoosi tehdään BMP-kaappauksilla.

**16.11. tehty:** uudelleentulosuoja lisätty (`CreateGuard`, thread-local-laskuri) molempiin vienteihin
`D3D11CreateDeviceAndSwapChain` ja `D3D11CreateDevice`; sisäkkäinen kutsu palauttaa `E_FAIL` ja kirjaa lokiin
`nested call … refused`. Käännetty (`proxy\build\out\Release\d3d11.dll`, SHA-256 48f3d7bf…), **ei vielä
asennettu peliin eikä testattu**. Suoja estää vain pinon ylivuodon; RenderDoc-käynnistys ei silti todennäköisesti
toimi, joten sitä ei käytetä.

**16.11. klo 11.11 session testitulos:** suojattu d3d11.dll (48f3d7bf…) + repo-ini (82978ee3…) toimivat:
OpenVR-init läpi, FSR1 käännetty, 26 000+ Presentia, `submitErr=0`, 0 `nested call`-riviä, 0 kaatumista.
Pään asento LIIKKUI tällä kertaa (plugin-lokin hmd-arvot 11.12.54–11.13.57; edellisen session huoli ei toistunut).
Silmävuorottelu näkyy (eye=0/1, ±2.2 yks. IPD-offsetit). Pelin latauduttua per-silmä bounds asetettiin (frame 3319).
Käyttäjä ei ehtinyt testata eikä tähdätä (kaikki zoom=0, adjustCur=0). `stereo patch`-rivit kirjaavat jatkuvasti
`t=infs cam=-nan` – merkitty tarkasteltavaksi. Huom: Vortexin deploy-manifestissa ei ole meidän tiedostoja
ja F4_Flat-paketti ei ole enää stagingissa → aamun 11.02 ylikirjoitus oli todennäköisesti vanhentunut
deploy-jono, nyt purkautunut.

- Burst #3 IPD=0 (14:53:03, 4487×2524, 16:9-istunto): X1 (L_k vs R_k) ≈ 0 (0, ±5; poikkeukset −36/−11 vain
  liikkeessä) → L- ja R-frames identtisiä kuten configin pitää. X2 ≈ 0 (poikkeumat k=4 −168, k=12 +124,
  k=20 −207 = liikahetkiä). Lähetepuoli noudattaa IPD=0:tä täsmälleen. PISTOOLIRAJAUS 4487-leveälle framelle:
  x 2650–3400, y 1850–2500 (esikatselu ×5).
- Käyttäjän PS (IPD=0-ajo): ADS:ssä aseen tähtäys tuntuu sidotun siihen mihin katsoo (head-centered mono-aim
  = TARKOITETTU), ja tähtäys on hieman väärin vaikka tähtäisi oikein → yhteensopiva adjustCur=0.000-ongelman
  kanssa (fovAdjust-luenta rikki 1.11.240:ssä; aim-offset korjaus odottaa).

**16.11 klo ~16:4x mallien 3D-tuntua + kaappausten poisto (config-only, testikierros):**
- Käyttäjän palaute (istunto 15:21, uusi plugin aim_headpos=false aktiivinen): syvyys TUNTUU (IPD-palautus
  toimi). Valitukset: (1) pelin käynnistys tekee kuva-kaappaukset (burst) — poistetaan; (2) tähtäys ampuu
  yhä katseen mukaan; (3) hahmon ja aseen mallit näyttävät kovin 2D:ltä yleensäkin.
- Syitä logista: session vr_cam_states-maski oli 0x1 = VAIN first person stereona → 3. persoonan näkymä
  kulkee proxy 2D-ruutipolun kautta (flat screen HMD:ssä) → hahmomalli FLAT. ADS:n ase on mono
  (aim_ipd_scale=0) → ase 2D ADS:ssä. Molemmat "by design" mutta ei-toivottuja käyttäjälle.
  Huom: aim-transitiota EI logittunut kertaakaan 15:21-sessiossa → ADS-tunnistus ei laukesin kyseisessä
  sessiossa (tai käyttäjä ei ADSannut) — fovscan ADS-dumppia ei siis vielä saatu; tarvitaan ADS ~2-3 s.
- CONFIG-MUUTOKSET live-iniin (peli ei käynnissä):
  capture_aim_frames=0 (kaappaukset pois),
  aim_ipd_scale=0.35 (ADS-ase saa osittaisen stereon, 35 % IPD:stä — riski: jos Link/VD-compositorin
  L/R-sekoitus koskee myös ADS:tä, tupla voi palata ADS:ssä → palauta 0 jos näin),
  vr_cam_states=0,7 (3. persoonan state 7 mukaan stereoon → hahmomalli 3D; aim-handling on
  state-0-only joten vanha "sideways mono" -ongelma ei uusile).
- "Ampuu katseen mukaan": ADS-näkymä on head-rotatoitu ja tähtäin on näkymän keskellä; aim_headpos=0
  linjaa tähtäinlaitteet tähtäimeen. Luonteeltaan tähtäys = katsominen tässä arkkitehtuurissa (ei
  kontrolleripose-seurantaa). Jos osumakohta yhä poikkeaa tähtäinlaitteiden osoittamasta pisteestä,
  tarvitaan konkreettinen testi: ADS → etutähtäin merkkiin → laukaus → osuuko ammus merkkiin?

**16.11 klo ~16:3x ADS-tähtäys + fovAdjust-diagnoosi (uusi plugin-build):**
- Selvitetty syvyyden puute: käyttäjän VD-sessiot (burstit 15:05/15:11) ajoivat YHÄ IPD=0-configilla
  (IPD palautettiin vasta klo ~16:1x) → maailma oli suorakulmaisesti flatti. Syvyystereo palautuu
  seuraavassa ajossa (ipd_units=4.48 + use_runtime_ipd=1 vahvistettu). ADS:n ase on yhä tarkoituksella
  mono (aim_ipd_scale=0) — hipfiressä sillä on stereo + konvergenssi.
- ADS-tähtäyspoikkeaman mekanismi: ADS:ssä kameraan sovellettiin yhä PÄÄN SIJAINTIA (6-DOF), mutta ase
  mallinnetaan pelaajan runkoon → pään sijaintipoikkeama irrottaa tähtäinlaitteet tähtäimestä.
  FIX: uusi config `[engine] aim_headpos=0` (oletus) → ADS:ssä (aimFp, state 0) pääSIJAINTI ohitetaan,
  pääKIERTO seuraa yhä HMD:tä. Hipfire säilyttää 6-DOF:n. (fo4stereo.ini.bak_prelugi -varmuuskopio.)
- fovAdjust=0-diagnoosi: lisätty "fovscan"-logi (dumpaa PlayerCamera-float-ikkunan 0x150..0x1B0 24
  arvona 1×/s). Vertaamalla hipfire- ja ADS-dumppia löydetään kenttä joka animoituu ~1.0 → ~0.75 ADS:ssä
  = oikea fovAdjustCurrent-offset runtime 1.11.240:ssä (CommonLibF4:n 0x170 lukee 0). Kun offset löytyy,
  aim-tunnistus siirtyy moottoripuolelle (nopea ADS-toggle, ei proxy-heuristiikkaa) ja adjustCur-logiin
  tulee oikea arvo.
- Plugin rakennettu (build_plugin.bat) ja asennettu; testi: ADS muutamaan kertaan (pysy ADS ~2 s),
  ammu seinään ja vertaa osumakohtaa tähtäimeen.

**16.11 klo ~16:0x RATKAISEVA TESTI: tupla poistui IPD=0:lla → Link-laittepuolen compositorin L/R-sekoitus VAHVISTETTU:**
- Käyttäjän havainto kypärästä IPD=0-ajossa: tupla POISTUI (kuva yksi, vain syvyystereo puuttui).
- Todistusketju: (1) X1≈0 mitattu → L/R-framet identtisiä lähetepuolella; (2) tupla katosi kypärästä.
  Eli tuplan lähde on silmäsisällön sekoitus, joka ilmenee VAIN kun L/R eroavat → Quest Link -kuljetuksen
  laitepuolen compositori (ATW) käsittelee AFR:n peräkkäiset L/R-presentit väärin. Ei meidän submit-ketjun
  eikä pelin eikä SteamVR:n PC-puolen vika. PC vr-view ja mirror aina puhtaita — täsmää.
- IPD palautettu (fo4stereo.ini.bak_preipd0 → fo4stereo.ini: ipd_units=4.48, use_runtime_ipd=1).
  capture_aim_frames=2 jää päälle (toimii edelleen varmistusmittana).
- MAHDOLLISET JATKOT (arvio):
  a) Oculus Debug Tool (Link): Asynchronous Timewarp / Spacewarp OFF → testaa normaali-IPD:llä. Halvin testi.
  b) Vaihtokuljetus: Steam Link -sovellus Quest 3:ssa Quest Linkin sijaan → jos tupla poistuu, syyllinen on
     yksiselitteisesti Quest Linkin ATW (erilainen kuljetus/compositori).
  c) Submit-mallin muutos proxyn puolelle: esim. submitataan molemmat silmät joka presentista (nykyinen
     eye-per-present) siten että parillisella presentilla tex[1] (vanha R) submitataan R-silmälle samalla kun
     tex[0] L:lle — SteamVR parittaa jo "viimeisimmät", mutta Link saatetaan nähdä sen eri tavalla. Koodimuutos.
  d) Kompromissi: pienennä IPD:tä (esim. ipd_units=2) → tuplan siirtymä pienenee puolikkaaksi; ei korjaus.

**16.11 klo ~15:3x burst #2 analysoitu: SILMÄPARITUS OIKEIN; tupla = post-submit, laitepuolen compositorin epäily:**
- Burst 14:44:23 (hipfire, 40 framea × 2 tekstuuria). Diff: sama duplicaatiokuvio (tex[0] päivittyy joka toinen
  present, A=0.00 parillisilla; liikettä enemmän kuin aamulla, A 14–43).
- **Ristikorrelaatio pistoolialueella (uusi ShiftUtil-työkalu):**
  X1 (L_k vs R_k) = d* −74…−84 px VAKIO etumerkki → tex[0]/tex[1] pitävät johdonmukaisesti eri silmiä
  (IPD-parallaksi). X2 (L_k vs L_{k+2}) = ≈0 → tex[0] EI vaihda silmää. POIKKEUKSET k=8 (+100), k=20 (−165)
  vastaavat diff-lokin liikahduksia (A=43 k=20).
- PÄÄTELMÄ: submit-ketju on paritukseltaan terve. Tupla (vain HMD:ssä, ei PC-mirrorissa) syntyy submitin
  JÄLKEEN. Käyttäjällä Quest Link -kaapeli + 90 Hz → Questin laitepuolen compositori (ATW) ei näy PC-näkymässä;
  se käsittelee AFR:n peräkkäiset L/R-presentit yhtenä virtana → aseen ±37 px silmäparallaksi sekoittuu
  yhden silmän kuvaksi → tupla vain kypärässä. SteamVR Motion Smoothing OFF ei auta (asetus koskee PC-puolta).
- TESTI PANNAS TALTEEN: IPD=0 (use_runtime_ipd=0, ipd_units=0, varmuuskopio fo4stereo.ini.bak_preipd0).
  Jos tupla katoaa kypärässä kun L/R-frames ovat identtisiä → compositorin L/R-sekoitus vahvistettu →
  ratkaisuja: (a) submit-mallin muutos, (b) Link-kompositorin asetukset, (c) hyväksyttävä kompromissi.
  Jos tupla jää → syy muualla (linssit/IPD-hardware, SteamVR-reprojektio). capture_aim_frames=2 jää päälle
  → uusi burst toimii myös varmistuksena että X1≈0 kun IPD=0.
- Käyttäjän PS: ADS toimii nyt rikkoutumatta. Ase näyttää "2D-mallilta" ADS:ssä = TARKOITETTU (mono-aim,
  aim_ipd_scale=0 → molemmat silmät identtiset). Tähtäyspiste hieman väärin = tunnettu adjustCur=0.000-ongelma
  (1.11.240:n rikkonut fovAdjust-luenta) — erillinen korjaus PLANissa.

**16.11 klo ~15:0x harmaan maailman juurisyy löytyi (kuollut koodi nykyisessä buildissa):**
- Oire: kypärässä näkyi vain hahmo + ase, muu maailma harmaata. Proxy-logi paljasti: `vr: init failed err=108
  (headset not found)` — peli käynnistettiin ilman aktiivista HMD:tä, JA `stereo patch #N t=infs shift=-nan
  cam=(-nan...)` — stereo-patch ajoi NaN-datoilla.
- Juurisyy: `QueryPerformanceFrequency`/`g_qpcStart`/`LoadStereoConfig` olivat päätyneet orvoiksi KUOLLEEKSI
  koodiksi SaveTextureBmp:n `return true`:n JÄLKEEN (commitoimaton muokkaus; git-diff vahvistaa). Niinpä
  g_qpf=0 → t=inf → shift=NaN, ja LoadStereoConfig ei koskaan ajanut → g_stereoEnabled jäi oletukseksi TRUE
  → 752B-kamera-puskurin patch kirjoitti NaN-matriiseja maailman VP:hen → maailma katosi. Hahmo+ase käyttävät
  eri puskureita → jäivät näkyviin. AIEMMAT buildit (48f3, C02BB) toimivat — bugi tuli tähän päivän buildiin.
- Korjattu proxy.cpp (build 30939728…, asennettu): 1) kuollut koodi poistettu SaveTextureBmp:stä, 2) alustus
  siirretty laiskaan one-time-initiin HookedPresentin alkuun (suoritetaan ensimmäisessä presentissa millä
  tahansa polulla), 3) lisäsuojaksi `isfinite(g_frameShift)`-gate PatchCameraBufferiin.
- Opsuositus: käynnistä peli VAIN kun SteamVR on päällä ja HMD aktiivinen (Quest Link yhdistetty) — err=108
  ei ole pelin eikä pluginin vika, vaan VR-yhteyden puute. Plugin sovelsi silmäsiirtoa pose=false -tilassakin
  (±2.24 yks.) — erillinen gate pluginiin mahdollinen myöhemmin, ei kiireellinen.

**16.11 klo ~14:0x jatko-suunnitelma vanilla-pohjalta (käyttäjän päätös):**
- Tavoite: varmistaa ettei mikään vanha asetus/mod häiritse; jatketaan kehitystä VAIN omalla modilla
  (F4SE-plugin + d3d11-proxy), ei muita modeja. RenderDoc otetaan uudelleen harkintaan nyt kun data on vanilla.
- Asennusjärjestys kun Fallout4.exe on palautunut: 1) F4SE versio joka sopii asennettuun runtimeen (tarkista
  fose.silverlock.org, runtime-versio Data/Fallout4.exe:stä), 2) oma F4SE-plugin dll (Data\F4SE\Plugins),
  3) proxy d3d11.dll repo-buildista (702D67E7…, CreateGuard + burst-kaappaus mukana) + config\fo4stereo.ini,
  4) baseline-testi ilman muita modeja (hipfire-stereo OK?), 5) burst-uudelleenajo (capture_aim_frames=2) →
  pistoolialueen vaakasiirtymäkorrelaatio ratkaisee kumman silmän renderin tex[0] pitää,
  6) RenderDoc: F11 in-process -kaappaus (renderdoc.dll prosessissa laukaisulla RenderDoc UI:n kautta — EI
  vanhaa capture_renderdoc.ps1-käynnistystä; CreateGuard estää rekursion). Poista Steam-overlay muuttujaksi
  (Steam → Asetukset → Pelin sisäinen → Overlay pois) RenderDoc-ajojen ajaksi. RenderDoc antaa piirustustason
  näkyvyyden: nähdään suoraan piirtääkö engine aseen kahdesti ADS-frameen.
- Muistutus: Vortex ei saa deployata mitään meidän tiedostoista (aiempi stale-deploy-välikohtaus); pidä
  meidän asennukset suorina kopioina pelikansioon.

**16.11 klo ~13:45 burst-analyysi valmis + pelikansio palautettu vanillaksi (käyttäjän toimesta):**
- Käyttäjä palautti pelin vanilla-asetuksiin ilman F4SE:ä ym. modeja → kaikki asennetut tiedostot poistuivat
  pelikansiosta (d3d11.dll, fo4stereo.ini + bakit, proxy-logi, burst-BMP:t). REPO EHJÄ: lähdekoodi, build
  (proxy\build\out\Release\d3d11.dll, SHA 702D67E7…), config\fo4stereo.ini, kaikki dokumentit.
- **Burst-diff-tulos (80 BMP:tä analysoitu ennen poistoa — tulokset tallessa):** tex[0]-tekstuurin sisältö
  päivittyy vain joka toisella presenta ja pysyy TÄSMÄLLEEN identtisenä välissä (diff=0.00 yli 6 Mpix:n alalla,
  joka toinen naapuripari). Tämä on AFR:n ODOTETTU malli yhdellä backbufferilla: tekstuuri pitää samaa silmää
  vastaavaa renderiä kaksi presentia. Magnitudit: A(peräkkäiset held-sisällöt) 2.5–5.5 ≈ puolet
  C(silmien välinen samassa framessa) 5–6. Liikahdushetket k=13–22 (A jopa 13.4) = pään/aseen liike.
- Avoin kysymys jäi: kumman silmän rendereitä tex[0] pitää (oikea paritus / vaihdettu / sama silmä) — se
  ratkaistaisiin vaakasiirtymäkorrelaatiolla pistoolialueella (d* etumerkki), joka vaatii uuden burstin.
  JOS modi asennetaan uudelleen puhtaalta pohjalta, burst-testi (capture_aim_frames=2) on ensimmäinen diagnostiikka.
- Huomio palautuksen yhteydestä: appmanifest/temp kosketettu ~13:45 → Steam toimi; burst-tulokset eivät
  selitä itseään uudelleen ilman vertailua vanilla-tilassa, mutta data on kirjattu.

**16.11 klo ~13:4x eye_delay=0-tulos + burst-kaappaus valmisteltu:**
- Käyttäjä: tupla EI poistunut eye_delay=0:lla, mutta "paras yritys tähän mennessä" → staattinen paritus on
  lähellä oikeaa mutta ei riitä. Loki vahvistaa: `vr config: eye_delay=0` luettu; vakaassa tilassa presents ja
  updates kulkevat täsmälleen 1:1 (+150/+150 / 2 s). → Ei kiinteä off-by-one vaan PUTKEN SYVYYDEN AJAUTUMINEN
  (render-threadin viive vaihtelee → staattinen viivevalinta lipsahtaa ajoittain = Starfield-mallin drift).
- Uusi työkalu: `capture_aim_frames=2` = burst-moodi (proxy.cpp). Ensimmäisen vakaan hipfire-stereon (seq>0,
  150 vakaata) jälkeen tallentaa 40 peräkkäistä presenta KUMMATKIN silmätekstuurit (80 BMP:tä ~1,9 GB,
  peli takkua tallennuksen ajan). Yksittäinen BMP ei voi näyttää monokulaarista flippiä (jokainen frame on
  yksittäisenä puhdas), mutta peräkkäisten framien pikselivertailu SAMASTA tekstuurista voi: jos g_eyeTex[0]
  vuorottelee vas/oik-renderien välillä, ase hyppää ~IPD:n verran naapuriframejen välillä.
  Build: d3d11.dll (uusi hash alla asennuksen jälkeen). Offlinenalyysi: PowerShell + System.Drawing,
  peräkkäisten framien diffi.
- Tulkintamatriisi: (a) g_eyeTex[0] naapuriframejen sisältö hyppää kahden tilan välillä → flip TODISTETTU →
  lisätään Starfield-mallinen laskuripohjainen skip-present-korjaus; (b) naapuriframet identtiset (still) →
  flip ei ole tex[0]:ssa → epäily siirtyy submit-assignmentiin / compositoriin; (c) hyppy jatkuva ja pieni →
  idle-sway + head motion — analysoidaan regionittain.

**16.11 klo ~13:0x vrframework-guidet 07 + 10 luettu (linkit tutkittu):**
- Guide 07: AFR-pariteetti = illuusion perusta; kolme laskuria (engine/render/presenter) — meillä pluginin
  update-seq vs proxyn present-seq; pariteetin lipsahdus = juuri meidän off-by-one-epäilymme. Korjausmalli
  (Starfield): warm-up >100 framea + lipsahdettaessa skip-next-present (pudota yksi present → pariteetti
  snapaa takaisin). REFrameworkin `m_left_eye_interval` on mutatoitava juuri tätä varten = meidän
  eye_delay/swap_eyes-ini-käännökset ovat guidein suosittelema ratkaisumuoto.
- Guide 10: TAA/AFR-saastuminen ("previous frame" = toinen silmä) olisi isoin ghost-luokka, MUTTA meidän
  BMP:t todistavat still-framen puhtaaksi → TAA ei selitä monokulaarista flippiä; ADS/mono on effectively
  same-eye → TAA-suhteessa puhtain. Submit-puoli (kopio, triple-buffer, bounds) jo oikein.
- Tärkein käytännön nyrkkisääntö molemmista: **varmista laskureilla, ei silmillä** — lisää lokiin per-frame-
  sisältövertailu (tex[0] vs tex[1]) jos ini-käännökset (eye_delay=0/swap_eyes) eivät ratkaise.
- KÄYTTÄJÄN TARKENNUS: aiemmin "normaali kuva toimi oikein, ei tuplakuvaa" → hipfire-stereo on AIEMMIN
  toiminut eye_delay=1:llä → nykyinen hipfire-flippi on uutta tai muuttunut (present/update-rytmi).
  eye_delay=0-testin tulos ratkaisee suunnan; jos molemmat arvot tuplaavat, rytmi ajautuu keskellä sessiota
  → tarvitaan drift-tunnistus/skip-present-malli tai sisältövertailuloki.

**16.11 klo ~12.3x diagnoosi lukittui (käyttäjän wink/mirror-testit):**
- Mirror = YKSI kuva ja siinä tuplapistooli. Hipfire tuplaa myös (oli varmasti ennenkin — motion smoothing EI syy).
- Wink-testi: tupla säilyy yhdelläkin silmällä → MONOKULAARINEN ajallinen flip, ei diplopiaa.
- BMP:t puhdas → yksittäinen frame on yksittäinen → joku vaihtaa silmätekstuurin sisältöä framelta framelle.
- Syyhypoteesi (koodianalyysi + plugin-loki): silmäparitus off-by-one. `eye_delay=1` vie uusimman renderin
  edellisen silmän tekstuuriin → `g_eyeTex[0]` vuorottelee vas/oik rendereitä → lähin objekti (pistooli)
  hyppää IPD:n verran 30 Hz → sinnikesto = tuplapistooli yhdellä silmällä; tausta barely liikkuu.
  Plugin-loki tukee: update presenta 3319 jälkeen eye=0, presenta 3320 eye=1 (render = uusin update).
- TESTI PANNAS TALTEEN: pelikansion ini eye_delay=1 → 0 (ei rebuildia). Käyttäjä: relaunch → hipfire wink-testi.
  Jos flip poistuu → silmäparitus korjattu; ADS-tupla todennäköisesti sama ilmiö. Jos ei → kokeillaan
  swap_eyes=1 (eye_delay=0). Repo-config päivitetään vasta kun oikea arvo varmistunut.

**16.11 klo ~12.15 toinen kaappauserä (150 framen viive, C02BB377-build):** molemmat kuvat PUAHTAITA.
- Hipfire (seq=150): yksi risti + ase, ei haamuja. ADS still (seq=1382): yksi tähtäin, yksi ase — aiempi
  "haamu" oli ADS-animaation liikehindu, EI pysyvä tuplarenderv.
- Loki: ADS:n aikana molemmille silmille identtinen tekstuuri + identtiset bounds (u=0.075..0.925) →
  stereoero = 0 submitissä.
- **Ristiriita:** käyttäjä raportoi PYSYVÄN tuplakuvan ADS:ssä, mutta lähetetty kuva on yksittäinen ja
  silmien sisältö identtinen. Päätelmä: tupla syntyy submitin JÄLKEEN (SteamVR compositor / Steam Link
  reprojektio / linssit/IPD) TAI käyttäjän "tupla" on eri ilmiö (esim. tähtäimen ja ristin mismatch,
  TAA-hindu maailmassa pään liikkuessa). Seuraava diagnostiikka: SteamVR desktop mirror ADS:n aikana —
  jos mirrorissa on tupla, vika on submit-inputissa (ristiriita BMP:iden kanssa); jos mirror on puhdas,
  vika on HMD/Steam Link -puolella. Kysyttävä: tuplaako ase vai koko maailma, näkyykö myös hipfiressä,
  liikkuuko tupla pään liikkeessä.

**16.11 klo ~11.40 ADS-BMP-tulos (ensimmäinen kaappausajo):** molemmat kaappaukset onnistuivat.
- ADS-tunnistus toimii: `zoom=1 (game tan x=1.303, ratio 0.947)` laukesi, mono-aim päälle, molemmat silmät
  samalla tekstuureilla ja samoilla boundsilla (`mono=1`, identtiset SHA-256:t aim_left/right BMP:issä).
- **Aim-BMP:ssä näkyy haamu:** kiinteä etu-/tähtäin, mutta läpikuultava asekopio eri kuvakulmasta (piipporimasto
  toistuu taakse- oikealle). Haamu on siis JO pelin lähettämässä tekstuurissa → ei pelkästään submit/bounds-vika.
  Epäselvää onko pysyvä tuplarendervai TAA/animaatiohindu: kaappaus laukeesi ~12 framea tunnistuksesta =
  ADS-animaation keskellä. Hipfire-BMP osui latausruutuun (seq=0, kamera ei päivity) → ei kelpaa vertailuun.
- **Korjaus tehty proxy.cpp:hen:** kaappausvakaus 12 → 150 framea (~2,5 s) ja ehto `seq > 0` (ei latausruutuun).
  Käännetty (`proxy\build\out\Release\d3d11.dll`, SHA-256 C02BB377…), **EI VIELÄ ASENNETTU** (peli oli käynnissä;
  edellinen asennettu on 48f3d7bf…). **Asennettu klo ~11.5x** (peli suljettu, varmuuskopio
  `d3d11.dll.bak_guard48f3` = 48f3d7bf…). Vanhat BMP:t poistettu, aim-kuvasta tallennettu pienennetty kopio
  temp-kansioon.
- **Käyttäjän palautte: tuplakuva on PYSYVÄ tähtäyksen aikana** (ei katoa kun asento on liikkumaton) → ei TAA-hindu.
  Päähypoteesi nyt: samaan flat-frameen päätyy aseen piirto kahdella eri silmäoffsetilla (stereo-patch osuu aseen
  piirtokutsuihin eri silmän matriiseilla silmävuorottelun takia; haamun eri kuvakulma sopii tähän). Seuraava
  kaappauserä (150 framen viiveellä) kertoo: pysyykö haamu still-ADS:ssä (→ tuplarenderv) ja onko offsetti
  ~silmäparallaksi (→ molemmat silmät yhdessä kuvassa). Hipfire-still-kaappaus kertoo esiintyykö haamu myös ilman ADS:ää.

**16.11 klo 16:00 ADS-BMP-testi valmisteltu:** peli suljettu, pelikansion `fo4stereo.ini` →
`capture_aim_frames=1` (muut arvot ennallaan). Kaappaus koodissa: yksi BMP-pari (g_eyeTex[0]/[1], 2433×2524,
suoraan SteamVR:ään lähetettävät tekstuureille) hipfire-tilasta ja yksi ADS-tilasta (laukeaa 12 vakaan framen
jäljen jälkeen, kerran per tila per sessio), tiedostot pelikansioon `fo4stereo_capture_*_left/right.bmp`.
Käyttäjä testaa: lataa peliin → hipfire 2–3 s (tallentuu automaattisesti) → käännä päätä → ADS 2–3 s → "lue kaappaukset".

**16.11. klo 11.05 normaali käynnistys (ilman RenderDocia) toimii:** peli pyörii vakaasti (8000+ framea,
`submitErr=0`, ADS-tunnistus `zoom=1` laukeaa, ei kaatumisia) → RenderDoc-käynnistys oli kaatumisen ainoa syy.
**Huom Vortex:** Vortex ylikirjoitti klo 11.02.28 pelikansion `d3d11.dll`:n vanhalla 0.3-paketin versiolla
(270 848 t, ilman suojaa) ja `fo4stereo.ini`:n vanhalla (upscale=0). Käsin asennettu suojattu build ei siis ollut
käytössä. Asennus pitää tehdä Vortexin staging-kansioon / rakentaa FOMOD 0.3.5, tai Vortex-deploy ohittaa
(käynnistys ilman Vortexia), muuten se palauttaa vanhat tiedostot jokaisessa käynnistyksessä.

**(Alkuperäinen toimenpide-ehdotus, nyt osittain ylitetty):** analysoi dump 23228 (WinDbg/cdb, `!analyze -v`, moduulilista), lue proxy.cpp:n CreateDevice /
CreateDXGIFactory1 -polku ja RenderDoc-API-lataus. Jos syy ei löydy nopeasti: poista RenderDoc-API-koodi
käytöstä (käännösaikainen `#if`, oletus pois) ja palauta tunnettu hyvä build
(`archive\…\d3d11.dll.before-0.3.4`, `…before-capture-support`) kunnes regressio on eristetty.

**16.11 klo 20.2x – keskustelukamera VR:ään (dialogue = kamera-state 12):**
- Kamera-statet (CommonLibF4): 0 FirstPerson, 2 VATS, 5 PCTransition, 6 Tween, 7 Animated (jakaa vtablen
  3. persoonan 8:n kanssa → raportoituu 7:nä), 9 Furniture, 12 Dialogue. Pluginin lokista: state 12 on oma
  vtablensa (+2522B40) ja koukutettu → sama mekanismi kuin 3. persoonalla (`vr_cam_states`-maski).
- Muutos: `vr_cam_states=0,7,12` (repo `config\fo4stereo.ini` + live-ini; varmuuskopio
  `fo4stereo.ini.bak_predialog`). Plugin kirjaa nyt jokaisen kamera-staten vaihdon
  (`camera state A -> B (inVR=…)`), jotta oikea dialogi-state varmistuu lokista ja voidaan lisätä esim. 9.
  Plugin build (SHA 100cb1ee…) asennettu, edellinen `fo4flat_probe.dll.bak_predialog` pelikansion juuressa.
- Riski: dialogikamera tekee leikkauksia (shot-vaihdot), joten pään asento on suhteessa kunkin leikkauksen
  kameraan; jos huimaa, poista 12 maskista (ei koodimuutosta).

**16.11 klo 20.3x–20.5x – dialogi-testi + ADS-tähtäys katseessa:**
- Loki (20.30-sessio): dialogi = state 12, `inVR=1` ✅ (maski 0x1081). State 0↔7 vaihtelee 3. persoonaa
  vaihdettaessa; 7→2 = VATS (2D). Dialogin aikana proxy näkee kapean FOV:n (`shmZoom=1`, world FOV 31.5),
  joten tähtäystunnistus laukeaa myös dialogissa – aim-logiikka on kuitenkin vain state 0:ssa (`aimFp`).
- Tässä sessiossa EI ollut yhtään ensimmäisen persoonan ADS:ää (`aimFp=1` ei kertaakaan); ADS-lokit tulivat
  state 7:stä. Käyttäjän ADS-valitus ("tähtäys katseessa, ei piipussa; ase 2D") koskee ensimmäisen persoonan ADS:ää.
- Syy "tähtäys katseessa": pään kierto lisätään render-kameraan (GetRotation), mutta ase/tähtäin ja luodin rata
  seuraavat pelin omaa tähtäyssuuntaa → katse irrottaa tähtäimen piipusta. Aiempi `aim_headpos=0` poisti vain
  pään SIJAINNIN. **Korjaus:** uusi `[engine] aim_headrot=0` (oletus): ensimmäisen persoonan ADS:ssä pään kierto
  häivytetään identiteetiksi (~0,1 s), jolloin näkymä = pelin oma tähtäysnäkymä; vapautettaessa häivytys takaisin.
  Seuraus: ADS:ssä näkymä on pään suhteen lukittu (kuin litteä näyttö) – tarkoituksella. `aim_headrot=1` palauttaa
  vanhan. Plugin SHA 3818c431…, asennettu; varmuuskopio `fo4flat_probe.dll.bak_preaimrot`. **Testaamatta.**
- "Ase 2D-mallina ADS:ssä": tarkoituksellinen kompromissi (mono-aim: molemmat silmät identtinen kuva), koska
  vuorottelu-stereo + Quest Linkin L/R-sekoitus tuplaa lähikohteen. Oikea korjaus on tuplakuvan juurisyy
  (Link-compositor / submit-malli, ks. yllä a–d) – ei ratkaistu tässä.

**16.11 klo ~21.0x – käyttäjän palaute `aim_headrot=0`:sta + dialogi/HUD-pyynnöt:**
- ADS: tähtäys tuntuu paremmalta, **linjassa** ✅ (7 × `aimFp=1` lokissa). Haitta: 1. persoonassa pieni
  kaksoiskuva palasi ("tultiin vähän pakkia"). Epäilyt: (a) ADS:ssä render-kamera ei seuraa päätä mutta submit
  merkitsee kuvan HMD-asennolla → Quest Linkin reprojektio (sama ATW-ongelma kuin tuplakuvassa) ; (b)
  `aim_ipd_scale=0.35` siirtää kameraa 0,78 yks. (≈1 cm) sivuun; (c) IPD palautettu (stereo takaisin hipfireen).
  Selvitettävä: tuliko kaksoiskuva hipfireen vai ADS:ään, ja liikkuessa vai paikallaan.
- Dialogi: uusi `[engine] dialogue_back_m=0.4` – kamera siirretään taaksepäin katseakselilla state 12:ssa.
  Ei törmäystestiä (REL::ID:t ovat rikki 1.11.240:ssä → ei raycastia): ahtaissa tiloissa kamera voi mennä seinän
  läpi. "Toinen kuvakulma jos seinä" vaatisi säteenheiton (Havok) tai pelin oman shot-valinnan ohjausta → erillinen tutkimus.
  Plugin asennettu, varmuuskopio `fo4flat_probe.dll.bak_predialogback`.
- HUD / dialogivalinnat liian lähellä ja sivulla – juurisyy: UI piirtyy samaan backbufferiin kuin maailma, ja
  per-silmä-rajaus (u 0..0.815 vs 0.185..1.0) tekee saman UI-pikselin parallaksista ≈ 18 % kuvan leveydestä →
  UI näkyy erittäin lähellä ja reunoilla. Korjaus vaatii UI:n erottamisen omaan kerrokseen (proxy): (1) tunnista
  UI-passin alku (Scaleform-piirto, OMSetRenderTargets/viimeinen backbuffer-sidonta), (2) ohjaa UI erilliseen
  RGBA-tekstuuriin, (3) näytä se IVROverlay-paneelina mukavalla etäisyydellä (≈1,5–2 m) ja koolla tai
  sen kaltaisena valikko-ruutuna, (4) maailma submitataan ilman UI:ta. Ensimmäinen askel: instrumentointi, joka
  kirjaa mitkä render targetit/draw-kutsut ajetaan Presentia edeltävässä passissa dialogin aikana.

**16.11 klo ~21.1x – hipfire-kaksoiskuva: submit pose-tiedolla (`submit_with_pose=1`):**
- Käyttäjä: kaksoiskuva palasi 1. persoonan HIPFIREEN (ei ADS:ään). Juurisyy-hypoteesi koodista: proxy kutsuu
  WaitGetPoses joka present, ja compositor olettaa kumpikin silmätekstuurin renderöidyksi uusimmalla poseella.
  AFR:ssa toinen silmä on yhden tickin vanhempi → pään liikkeessä silmien välille syntyy lateraalinen ero
  (= tuplakuva) ja Quest Linkin reprojektio ei voi korjata sitä, koska se ei tiedä todellista renderipoosea.
- Muutos (proxy): jokaiselle silmätekstuurille tallennetaan edellisen WaitGetPosesin raakapose (= pose jonka
  pluginin kamerapäivitys luki), ja Submit tehdään `Submit_TextureWithPose` + `VRTextureWithPose_t`. Ei käytetä
  mono-ADS:ssä (näkymä on pään suhteen lukittu) eikä 2D-tilassa. Ini: `[vr] submit_with_pose=1` (0 = vanha).
  Build SHA tallennettu asennuksessa; varmuuskopiot `d3d11.dll.bak_prepose`, `fo4stereo.ini.bak_prepose`.
- Oletus: renderöintiviive = 1 pose (pluginin lukema pose on edellisen presentin WaitGetPoses). Jos viive onkin
  2, korjaus on vajaa; silloin lisätään plugin→proxy-kättely (pluginin käyttämä poseSeq shm:ään). **Testaamatta.**

**16.11 klo ~21.2x – palaute: hipfire OK ✅, ADS-kohdistus OK ✅, ADS-tupla palasi, ase/kädet liian isot, dialogin skaala:**
- Hipfire-kaksoiskuva korjaantui (`submit_with_pose=1`) ✅. ADS: pään kierron lukitus toimii ja luoti menee oikein ✅.
- **ADS-tupla (head-locked sisältö):** kun render ei seuraa päätä mutta kuva lähetetään silmätekstuurina, compositor
  reprojisoi sen kuin se olisi maailmaan lukittu → tupla/värinä pään liikkeessä. Korjaus: `[vr] ads_overlay=1` –
  ADS (mono, zoom=1, state 0) näytetään HMD-suhteisena IVROverlay-neliönä (`ads_overlay_distance_m=4`, leveys
  = 2·tanX·etäisyys, FSR-upskaalattu silmätekstuuri lähteenä); silmille lähetetään musta. `0` = vanha polku.
- **Ase ja kädet liian isot:** loki `firstPerson=80.000` vaikka Custom.ini `fDefault1stPersonFOV=137` → moottori piirtää
  aseen 80°:n projektiolla 137°:n maailmaan (~3× liian iso). Plugin kirjoittaa nyt `firstPersonFOV = worldFOV`
  hipfiressä (`[engine] fp_fov_sync=1`). Perustuu kenttiin 0x168/0x16C (worldFOV/firstPersonFOV); **vaikutus varmistamatta**.
- **Dialogin skaala:** dialogin aikana moottori kaventaa FOV:n (worldFOV 31–35°, game tan x=0.153) ja proxyn bounds
  ei voi näyttää kapeaa kuvaa oikeassa kulmakoossa (kuva venytetään koko silmälle, bounds 0..1) → hahmot ~9× liian isoja.
  Plugin pakottaa nyt dialogin worldFOV:n normaaliksi (`dialogue_fov=0` = hipfiren FOV, esim. 137) → oikea kulmakoko;
  pienempi luku suurentaa hahmoja. Lisäksi `dialogue_back_m=0.4`. Tarkista proxy-lokista `game tan x` dialogissa (~1.377).
- **RenderDoc:** ei suositella nyt. Syy: proxy on nimeltään d3d11.dll, ja RenderDocin hookit kutsuvat sen takaisin
  (rekursio; CreateGuard estää vain kaatumisen, ei käynnistystä). Pelin pelaamista ei voi automatisoida VR:ssä
  (ohjaus vaatii kypäräsession). Vaihtoehto: oma automaattinen piirtotrace proxyyn (UI-passin tunnistus HUD:ia varten).

**16.11 klo 21.2x – testi 21.18–21.26 (ADS-overlay, FOV-korjaukset) + "Delete ei toimi":**
- ADS-overlay toimii teknisesti (`ADS shown as head-locked overlay`, submitErr=0). Aseen FOV: `firstPerson=137` ✅
  (fp_fov_sync vaikutti kenttään). Dialogin tan x nyt 0.83–0.98 (ennen 0.153) → noin 9× → ~1.4× liikaa; bounds ~koko silmä.
- Dialogi 21.21.31–21.22.51 (state 12), sen jälkeen 3. persoona 21.23.45–21.25.00.
- 21.25.22: kamerapäivitykset loppuivat (updates=14421 jäädytetty, presents jatkui) → proxy 2D-tilaan; peli sulkeutui
  ~21.26 (ei kaatumisvedosta). Syy ei selvinnyt lokista (valikko/lataus/jäätyminen?).
- **Recenter (Delete) ei koskaan rekisteröitynyt:** lokissa 0 × `vr: recenter:` koko sessiossa → näppäin ei saavuta proxya
  (GetAsyncKeyState). Korjaus: (1) tarkistetaan myös "painettu viimeisen kutsun jälkeen" -bitti, (2) uusi gamepad-yhdistelmä
  **L3+R3** (molemmat sauvapainallukset) XInputilla, (3) loki `vr: recenter requested (keyboard|gamepad L3+R3)`.
  Build asennettu (varmuuskopio `d3d11.dll.bak_prerecenter`). **Testaamatta.**

**16.11 klo ~21.3x – Delete = kuvankaappaus:**
- `renderdoc_capture_key=46` (Delete). Painallus kirjaa `vr: capture key 46 pressed (flat=… camState=…)` (diagnostiikka,
  koska Delete ei aiemmin rekisteröitynyt recenterille). Jos `renderdoc.dll` ei ole ladattu prosessiin (normaalitila),
  tallennetaan BMP:t `<peli>\fo4stereo_shots\<aika>_cam<state>_zoom<z>_<3D|ads|2D>_{left,right|screen}.bmp`
  (kaikissa kamera-tiloissa, myös dialogi/HUD). Oikea RenderDoc-kehyskaappaus vain jos renderdoc.dll on ladattu
  (käynnistys RenderDocin alla ei vielä toimi, ks. rekursio).
- Recenter siirretty: `recenter_key=35` (End) + gamepad L3+R3.
- Build asennettu (varmuuskopio `d3d11.dll.bak_preshot`). **Testaamatta.**

**17.11 klo 7.4x – ensimmäinen hipfire-kuvapari (Delete, `fo4stereo_shots`, 2433×2524/silmä):**
- Kuvat vahvistavat: stereoparallaksi on oikea (pistooli/seinä siirtyvät silmien välillä), FSR-kuva terävä, aseen koko
  uskottava (`fp_fov_sync` toimii). Pip-Boy-käsi näkyy isona alareunassa.
- **HUD-juurisyy näkyy suoraan kuvasta:** HP-palkki + "Sanctuary" alavasemmassa reunassa, ammusmäärä (003/006)
  oikeassa reunassa, kompassi alakeskellä – kaikki molemmissa silmäkuvissa SAMASSA frame-paikassa. Per-silmä-rajaus
  (vasen u 0..0.81, oikea u 0.19..1.0) leikkaa reunat eri tavoin → HP näkyy vain vasemmalle, ammusmäärä vain oikealle
  silmälle (binokulaarinen kilpailu) ja loput elementit saavat suuren parallaksin → "liian lähellä ja sivulla".
  Korjaus vaatii UI:n erottamisen omaksi tasokseen (ks. suunnitelma yllä).

**17.11 klo ~7.5x – keskustelu jäädytettynä näyttönä ("What happened?" ei näkynyt kypärässä):**
- Syy kuvista: vastausvaihtoehto on framen alakeskellä (v≈0.82); silmän näkyvä alareuna on rajattu (bounds v 0..0.845,
  silmä näkee ~55° ylös ja vain ~44° alas) → teksti on käytännössä linssin reunalla/ulkopuolella ja pieni. Sama koskee HP/kompassi (v≈0.94).
- Käyttäjän ohje: keskustelussa ei tarvitse katsoa sivuille → kamera voi olla jäädytetty kuten ADS:ssä.
- Toteutus: plugin `[engine] dialogue_freeze=1` (state 12: ei pään kiertoa/paikkaa, shift 0, ei konvergenssia →
  kaikki framet identtisiä) + proxy `[vr] dialogue_panel=1`: dialogi näytetään koko framena head-locked IVROverlay-
  ruutuna (`dialogue_panel_dist_m=1.5`, `_width_m=2.0` ≈ 67°, `_y_m=0`, `_crop=0,0,1,1`), silmille musta. Koko frame mahtuu
  näkyvään alueeseen → vaihtoehdot ja tekstitykset luettavissa. Säädöt iniin; `dialogue_freeze=0`/`dialogue_panel=0` palauttaa vanhan.
  Asennettu (varmuuskopiot `*.bak_predlg`). **Testaamatta.**

**17.11 klo ~7.5x – dialogi: palaute "kuva oli 2D, ei VR; valinnat isompia ja keskelle":**
- Kuva `cam12` 7.49.29 osoitti: kaikki 4 vastausvaihtoehtoa framen alakeskellä (u 0.36–0.63, v 0.82–0.87), pieniä.
  Kokoruutu-tila (`dialogue_panel_full=1`) on tarkoituksella litteä 2D, jota käyttäjä ei halunnut.
- Muutos: oletus `dialogue_panel_full=0` + `dialogue_freeze=0`: 3D-VR-kuva säilyy (pään seuranta kuin 3. persoonassa), ja
  vain valinta-alue (`dialogue_panel_crop=0.25,0.74,0.75,0.93`) näytetään suurennettuna paneelina keskellä
  (`dist 1.2 m`, `width 1.4 m` ≈ 60°, `y -0.05 m`). Teksti ≈ 1.6° korkea (aiemmin 0.8°). Säädöt iniin. Testaamatta.

**17.11 klo ~7.5x – palaute: ADS (1. persoona) yhä tupla, 3. persoonan tähtäys vielä tuplampi, muu VR ok:**
- ADS-overlay (head-locked) ei poistanut tuplaa → päähypoteesi "head-locked + reprojektio" ei yksin riitä. 3. persoonan
  tähtäys (tavallinen stereo + pose-submit) on pahempi → yhteistä on liikkuva pelin oma kamera (hiiri/sauva) +
  vuorotteleva stereo: toinen silmä on tickin vanhempi, ja pose-submit korjaa vain pään liikkeen, ei pelikameran liikettä
  (AFR:n perusrajoite). Vahva ehdokas: peli renderöi vain yhden silmän per tick.
- Uusi diagnostiikka: **Insert** vaihtaa ADS-esitystä livenä (`ads_mode` 0 mono-silmätekstuurit / 1 overlay / 2 stereo kuin hipfire;
  shm.adsMode → plugin vaihtaa tähtäyksen normaaliin vuorotteleviin silmäsiirtoihin ja konvergenssiin). Loki: `vr: ADS mode -> N`.
  Testi: sama tähtäys kolmella tilalla → mikä on vähiten tupla. Ei vaikuta 3. persoonan tähtäykseen (aina stereo).
- Jos kaikki tuplaavat: juurisyy on AFR + pelikameran liike → seuraava iso askel on "molemmat silmät samasta pelitilasta"
  (reprojektio toiselle silmälle syvyyspuskurista tai kaksi renderiä/tick). Testaamatta.

## 5. Siivottavaa / epäjohdonmukaisuutta

1. **Kolme lähdepuuta:** `C:\AI_Projektit\fo4stereo` (proxy) ja `fo4flat_probe` (plugin) ovat vanhentuneita
   (12.11.); repon `proxy\src` ja `plugin\src` ovat totuus (15.11.). AGENTS.md ja AGENT_GUIDE.md neuvovat
   edelleen vanhoihin polkuihin ja MCM-vapaata tilaa vastaan ristiriitaisesti. → Julista repo ainoaksi
   lähteeksi, päivitä AGENTS.md:n FO4-osio ja merkitse vanhat kansiot arkistoksi.
2. **DEVLOG.md (824 riviä)** sisältää ristiriitaisia kerroksia (MCM-tuki, joka on poistettu; "Aito stereo ei
   toimi" -vanhoja osioita; kaksi eri `Päivitetty`-otsikkoa). Tiivistä: nykytila ylös, historia alas, MCM-osiot
   merkitty "poistettu".
3. **INI-oletusten ristiriidat:** AGENT_GUIDE `pipboy_ratio=0.5`, `vr_cam_states=0,7` vs. repo `0.75`, `0`.
   Repon `config\fo4stereo.ini` on voimassa; dokumentoi yksi arvo ja syy.
4. **Repo-roskaa:** `$extract\`, `libRealSpace-upstream\` (aiheeton, arkistoitu ulkopuolinen repo – ei kuulu tänne),
   `build\` ja `proxy\build\` (kaksi build-kansiota), `tmp\`, `dist\stage-0.3…0.3.4` (5 versiota). Lisää
   .gitignoreen tai poista (vain käyttäjän luvalla).
5. **Commitoimaton työ:** 13 muokattua + poistettuja tiedostoja (MCM poisto, proxy/plugin-muutokset). Commit
   pienissä osissa kun peli taas käynnistyy; **push vain käyttäjän luvalla**.
6. README mainitsee "Cline" ja AI-huomautuksen kahdesti (rivit 119–120 ja 130–131) – yhdistä.
7. AGENTS.md:n ihosairaus-/BW2COLOR-osiot kuuluvat eri projekteihin; FO4-osio on vain lyhyt osoitin.

## 6. Työjärjestys

**Vaihe A – Vakaus (ennen kaikkea muuta)**
1. Kaatumisen juurisyy (kohta 4): dump-analyysi, proxy-silmukan eristys, tarvittaessa hyvä build takaisin.
   Peli kiinni ennen asennusta; varmuuskopio ennen korvausta.
2. Lisää proxyyn yksi yhteenvetoloki per sessio ja esto sille, että sama init-blokki voi toistua (laskuri).
3. Kun peli käynnistyy: kirjaa `fo4stereo_proxy.log`-alku (OpenVR init, bounds, runtime IPD) ja plugin-loki
   (`hooked`, aim-lukemat) tuoreeksi referenssiksi.

**Vaihe B – Perustoimintojen uudelleenvahvistus (käyttäjä testaa kypärällä, agentti lukee lokit)**
4. Hipfire: stereo OK, ei heiluntaa; recenter (Delete); 3. persoona 2D; Pip-Boy + VATS.
5. ADS-tuplakuva yhdellä muutoksella kerrallaan, tässä järjestyksessä:
   a. Lue `capture_aim_frames=1` -BMP-parit (vasen/oikea hipfire + ADS) – **ei RenderDocia**. Vertaa kuvien
      sisältö ja bounds; tämä kertoo onko tuplakuva pelin tuottamassa kuvassa vai Submit-vaiheessa.
   b. Jos kuva on jo tuplana pelin renderissä → syy on kamerapuolella (ADS-animaation viive/FOV-zoom,
      ase renderöidään eri kamerasta). Kokeile: tähtäyksessä pelkkä yksi silmä molempiin (kuva kopioidaan
      toisesta silmästä, bounds sama) ja poista aim_ipd_scale-mallit kokonaan.
   c. Jos BMP:t ovat kunnossa mutta kypärässä tuplana → ongelma on submitissä/bounds/VRTextureBounds tai
      Steam Linkin reprojektiossa.
6. Vahvista, että aim-tunnistus (plugin `fovAdjust`/proxyn tan-suhde) laukeaa; korjaa offset vain jos lokit
   näyttävät nollaa.

**Vaihe C – Laatu**
7. Silmävuorottelun parannus: "Synchronized Sequential" (kaksi renderiä samalla tickillä) vaatii tutkimuksen
   mikä hook ajetaan kerran per render; ensin mittaus `iPresentInterval`-kokeella (DEVLOGin ehdotus).
8. Kiikarit: lokista zoom-suhteet; `scope_2d`-asetus jos tarpeen.
9. Pip-Boy-säädöt käyttäjän palautteen mukaan.

**Vaihe D – Julkaisu ja pitkä aikaväli**
10. Dokumentaatiosiivous (kohta 5), commit osissa, pyydä lupa pushiin, uusi FOMOD (0.3.5) vasta kun A+B läpi.
11. Myöhemmin: ImGui-overlay, per-silmä depth submit, piilomaski, motion-ohjaimet (FRIK-ideat, ei GPL-koodia).

## 7. Periaatteet (opitut, älä toista)

- Älä paikkaa D3D-matriiseja; silmäsiirto vain moottorin kamerassa.
- Yksi muutos kerrallaan, testi, loki – ADS-ongelmaa on jahdattu monella samanaikaisella muutoksella ilman
  todistetta; jokaisella hypoteesilla pitää olla mitattava loki- tai kuvatodiste.
- Älä käynnistä peliä RenderDocin kautta, ennen kuin kaatumissyy on eristetty.
- Asenna DLL:t vain kun Fallout4.exe ei ole käynnissä; varmuuskopio aina ensin; push vain luvalla.
- Lue tiedosto ennen muokkausta (rinnakkaisia istuntoja on ollut).
