# Status: Fallout 4 flat -> VR (stereo + gamepad)

Päivitetty: 2026-11-15

## Nykytilanne: tähtäys ja MCM:n poistaminen (15.11.2026)

### MCM on arkistoitu

- MCM:n valikkotiedostot, käyttäjäasetukset ja käyttöönoton apuskripti on siirretty arkistoon. Plugin ja proxy eivät enää lue MCM-tiedostoja; asetusten lähde on `fo4stereo.ini`.
- Jaetun muistin asetuspuskuri poistettiin ja nimi nostettiin v4:ään, jotta vanha v3/MCM-prosessi ei voi sekoittua uuden pluginin ja proxyn välille.
- Pelikansion F4Flat2VR-kohtaiset MCM-tiedostot ja niiden varmuuskopiot sekä vanhat DLL:t ja INI varastoitiin `archive/retired-mcm/game-install-20261115/`-kansioon. Aktiivisesta `fo4stereo.ini`:stä säilytettiin muut asetukset; MCM:n käytössä ollut `bEyeDelay=1` siirrettiin `eye_delay=1`:ksi ja ADS-offset jätettiin neutraaliin arvoon `aim_ipd_scale=0`.
- MCM-vapaat DLL:t käännettiin, asennettiin ja SHA-256-tarkistettiin. FOMOD 0.3 rakennettiin ja tarkistettiin: paketissa ei ole MCM-tiedostoja ja molemmat XML-tiedostot ovat kelvollisia.
- Alla olevat MCM-merkinnät ovat kehityshistoriaa, eivät nykyisen version toimintoja. Aiempi väite CyberpunkVR:n vasen/keskellä/oikea MCM-silmävalinnasta oli virheellinen.

### Mitä vertailuprojekteista voidaan oikeasti päätellä

- [CyberpunkVR Portin `EngineStereo.cpp`](https://github.com/dariulone/cyberpunk-vr-port/blob/main/src/Stereo/EngineStereo.cpp) pyytää moottorilta oikeat vasemman ja oikean silmän kamerat, käyttää headsetin IPD:tä ja rinnakkaista konvergenssia. Se ei ole sama toteutus kuin tämän projektin vuorottelevat renderöintikehykset.
- [CyberpunkVR Portin `AdsEyeAlign.cpp`](https://github.com/dariulone/cyberpunk-vr-port/blob/main/src/Anim/AdsEyeAlign.cpp) kohdistaa ADS-animaation aseen ja käsivarsien poseerausta sighting-eye-silmään cyclopean-kamerakeskuksen sijasta. Tavallinen ase käyttää oikeaa silmää; vasempaan käteen siirretty ase vasenta. Tämä ei ole yleinen vasen/keskellä/oikea-kuva-asetus, eikä se siirrä koko maailmakameraa. Oppi on muokata aseen poseerausta, jos ase on ainoa väärässä kohdassa; se ei yksin korjaa taustan tuplautumista.
- [UEVR:n README](https://github.com/praydog/UEVR/blob/master/README.md) erottaa Native Stereo-, Synchronized Sequential- ja AFR-renderöinnin. UEVR:n `Aim Method` valitsee tähtäyksen suunnan lähteen (peli, pää tai ohjain), ei dominoivaa silmää. Synchronized Sequential on hyödyllinen vertailukohta, koska silmät renderöidään samalla pelitickillä; sen TAA voi silti haamuttaa.
- `D:\\Fallout 4 VR\\src` sisältää F4SE/OpenVR-rajapintoja ja työkaluja, ei Bethesda-pelin kameran tai ADS:n lähdekoodia. `Fallout4VR.exe` on käännetty moottori, joten tarkkaa sisäistä tähtäystoteutusta ei voi vahvistaa puretuista peliresursseista. Dokumentaatio ei siksi väitä, että Fallout 4 VR käyttäisi samaa ADS-korjausta kuin CyberpunkVR.

### Tämän projektin näyttö ja seuraava tekninen askel

- 15.11. ADS-loki vahvisti `zoom=1`, kamera-tilan 0 ja pluginissa `aimFp=1`, `shift=0`. Molempien silmien `Submit` palautti onnistumisen ja käytti samoja rajauksia, mutta myös samaa D3D-tekstuuriresurssia. Käyttäjän havainto tarkentui: vasemmalle sijoittunut kaksoiskuva näyttää aseen kokonaan, oikealle sijoittuneesta puuttuu noin sentti aseen vasemmasta reunasta.
- Proxy kopioi jo uusimman kuvan molempiin silmätekstuuripintoihin, mutta lähetti silti saman tekstuuriosoitteen molemmille `Submit`-kutsuille. Korjaus käyttää nyt erillistä silmätekstuuria kummassakin kutsussa; niiden pitäisi sisältää sama kaapattu kuva. Uusi 0.3.1-paketti ja pelibinaari on rakennettu. ADS-testi tarvitaan ennen kuin voidaan päätellä, poistuiko kaksoiskuva; jos ei, seuraavaksi kaapataan tekstuurin sisältö ennen Submitia.
- RenderDocin attach-ongelman kiertämiseksi lisättiin `[vr] capture_aim_frames=1`: ensimmäisen persoonan hipfire- ja ADS-tiloista tallentuu automaattisesti yksi vasen/oikea BMP-pari pelin juureen 12 vakaan Presentin jälkeen. Oletus on 0; nykyinen pelikansion INI asetetaan testin ajaksi arvoon 1. Kolmannen persoonan kuva on tarkoituksella 2D (`vr_cam_states=0`), joten se ei tuota stereovertailukaappausta.
- 15.11. Lisättiin RenderDocin in-process API -kaappaus: F11 tallentaa seuraavan D3D11-kehyksen `.rdc`-tiedostoksi, otsikoituna hipfire/ADS-tilan mukaan ja pelikansion `fo4stereo_renderdoc`-hakemistoon. Myöhäinen `renderdoccmd inject --PID` latasi RenderDoc-moduulin, mutta capture API ei käynnistynyt jo luodulla D3D11-laitteella. `capture_renderdoc.ps1` käynnistää nyt F4SE-loaderin RenderDocin `capture`-komennon alla lapsiprosessien hookkauksella ja tarkistaa, että `renderdoc.dll` latautui varsinaiseen `Fallout4.exe`:hen. RenderDocin API-otsake haetaan asennuksesta CMake-vaiheessa; DLL:ää ei linkitetä tai paketoida modin mukana.
- F11-testi vahvisti API:n latauksen, mutta jälki-injektoinnissa tarkka ja wildcard D3D11-laite/ikkuna-pari eivät käynnistäneet capturea. Lisättiin `SetActiveWindow` ja dokumentoitu wildcard (`NULL, NULL`) -varayritys; EndFrameCapture käyttää vastaavaa pariargumenttia. Uusi käynnistysreitti odottaa D3D11-laitteen alustusta ennen kuin F11-testiä jatketaan.
- 15.11.2026: RenderDocin pre-launch-yritys kaatoi pelin ennen kaappauksen valmistumista; `.rdc`-tiedostoja ei syntynyt. Dump analysoitiin paikallisesti WinDbgissä (`C:\Users\mipoi\AppData\Local\CrashDumps\Fallout4.exe.23228.dmp`). Poikkeus oli käyttöoikeusvirhe (`0xc0000005`) KERNELBASE-polussa; pino sisältää toistuvia `gameoverlayrenderer64.dll`-kutsuja sekä RenderDocin `RENDERDOC_CheckRemoteServerConnection`- ja DXGI-alustuskehyksiä. Steam Overlay oli käyttäjän mukaan poistettu käytöstä, mutta DLL oli silti ladattu. Tämä on näyttö moduulien yhteisesiintymisestä kaatumispinossa, ei varma todiste yksittäisestä syyllisestä.
- **Jatko koneen uudelleenkäynnistyksen jälkeen:** älä käynnistä peliä RenderDocin kautta ennen kuin Steam Overlayn ja RenderDocin yhteensopivuus on eristetty. Tarkista ensin, että peli ei käynnisty taustalla ja että Steam-overlayn tila on yhä pois päältä; selvitä sitten, miksi overlay-DLL latautuu. Tee seuraava testi vain hallitulla tavalla ja yksi muutos kerrallaan. RenderDoc-kaappauksia ei toistaiseksi ole.

## Tavoite

Flat Fallout 4 pelattavaksi VR-laseilla niin, että näkymä täyttää koko näkökentän ja ohjaus tapahtuu Xbox-ohjaimella. Stereo (aito kahden silmän kuva) on ensisijainen tavoite. Ohjainsilta on toissijainen.

## Mitä on tehty

### Oppi muista projekteista (suunnittelu)

Projekti on saanut inspiraatiota erityisesti [CyberpunkVR Port](https://github.com/dariulone/cyberpunk-vr-port) -projektista (MIT-lisenssi, tekijä dariulone ja yhteisö). Siitä voidaan oppia seuraavia suunnitteluratkaisuja, jotka ovat sovellettavissa Fallout 4:n flat-to-VR -portaukseen:

- **Aito stereo, ei uudelleenprojisointia:** toinen silmä on oma moottorin render-kamera, joka renderöidään omaksi tekstuurikseen omasta sijainnistaan ja omalla projisiolla.
- **AFW (Alternate Frame Warping):** silmät vuorottelevat kehyksissä, toisen silmän kuva rekonstruoidaan edellisen kehyksen syvyyden ja liikkeen perusteella (nopea, GPU-säästöinen).
- **Asymmetrinen projektio per silmä:** jokaisen silmän näkymä rajataan headsetin linssikentän mukaiseksi, ei symmetrinen frustiimi.
- **OpenXR head tracking:** HMD-asento injektoidaan render-polkuihin, per-eye depth-kerrokset (`XR_KHR_composition_layer_depth`) parantavat reprojektiota.
- **Natiivinen VR-HUD:** HUD-elementit sijoitetaan omille paneeleilleen, näkymän lukitus ja free-look-kartiot säädettävissä.
- **FSR1 upscale:** pelikuva skaalataan headsetin suosittelemaan per-silmä tarkkuuteen ei-temporaalisesti, mikä on VR-turvallista.

Lisäoppija (luettu reposton ajantasaisesta README:stä 13.11.2026), suoraan sovellettavissa tämän projektin tunnettuihin rajoitteisiin:

- **AFW + oma rekonstruktio (OSR):** silmät vuorottelevat kehyksissä, mutta toinen silmä rakennetaan edellisen kehyksen väristä, syvyydestä ja liikkeestä (~0,44 ms GPU), joten kehyksen hinta on ~yksi silmä eikä frameraate jakaudu kahtia. Tämä on suora vastaus meidän "silmät vuorottelevat → puolet frameraatesta" -rajoitteeseemme; me vain toimitamme vuorottelevat kehykset ilman rekonstruktiota.
- **Näkymät tunnistetaan nimellä, ei eliminoinnilla:** ei "kaikki mikä ei ole toinen silmä on MAIN", vaan virtuaalikameran nimen hash — eliminoiva tunnistus tuotti toisessa projektissa valkoisen toisen silmän ja jumit. Meidän proxy tunnistaa kameratilat indeksillä; sama oppi pätee Pip-Boy-kohteen valintaan (`pipboy_target_index`).
- **Molempien näkymien pitää sopia yhteen:** varjokaskadit, shader-kello, tuulitilavuus ja heijastusmarssi olivat rakenteita, jotka toinen näkymä rakensi omasta frustumistaan → vilkkuvat varjot ja tärisevä kasvillisuus. Ratkaisu: molemmat silmät lainaavat samoja rakenteita. Meidän ratkaisumme (silmäsiirto moottorin omassa kamerassa) välttää ongelman lähtökohtaisesti, mutta oppi kannattaa muistaa jos näkymät joskus erotetaan.
- **Upskalerille oma konteksti per silmä:** temporaaliset upskalerit (FSR2/DLSS) keräävät toisen silmän historiaa → IPD:n levyinen aave. Meidän FSR1 on ei-temporaalinen ja turvallinen, mutta jos joskus siirrytään FSR2/DLSS:ään, konteksti pitää jakaa per silmä.
- **Per-silmä syvyyskerros submitissä:** runtime-reprojektio tietää missä asiat ovat (me OpenVR:ssä tämä olisi `IVRCompositor::SubmitWithLayer` + depth).
- **GPU-säästöt:** piilomaski linssien reunoille, kiinteä foveaatio (FFR/VRS) ja jaetut auringonvarjot — pieni kustannus, iso voitto resoluutiorajoitteisessa kokoonpanossa.
- **Exposure mitataan ympäristöstä, ei kameran suunnasta:** valotus ei enää pyrähdy kun kääntää päätä. Fallout 4:n autoexposure-ominaisuudet voisivat hyötyä samasta periaatteesta.

**Credits:** dariulone (pääsuunnittelija), iPowerTech, dabinn, Ajson44, tig3rmast3r/OFXR Bridge, satyaloka93, Syncher-source, EffinexX (katso tarkemmat krediitit CyberpunkVR Port -repossa). Projektissa **ei ole kopioitu koodia** CyberpunkVR Portista, vaan se on toiminut viite- ja inspiraationa.

### MCM-tuki (13.11.2026)

- **MCM-valikko tehty** (MCM-ydin, mod 21497): `Data\MCM\Config\F4Flat2VR\` (repo: `mcm/F4Flat2VR/`).
  Kaksi sivua — *Stereo* (IPD, pään kierto/kompensointi/paikka, units/meter, aim-IPD, konvergenssi) ja
  *VR-näkymä* (eye_delay, swap_eyes, wait_poses, FSR1-terävöinti, 2D-ruudun koko/etäisyys, aim/pipboy-ratiot,
  Pip-Boyn koko/etäisyys). Oletusarvot `settings.ini`, tekstit suomeksi.
- **Sovellusprosessi:** MCM kirjoittaa käyttäjän asetukset `Data\MCM\Settings\F4Flat2VR.ini`; pluginin
  poll-säie (500 ms) lukee muutokset mtimen perusteella, soveltaa stereoarvot suoraan moottorikoukkuihin ja
  puskee VR-arvot jaetun muistin uuteen asetuslohkoon (shm v3 `StereoSettings`, seqlock), jonka proxy lukee
  jokaisen Presentin alussa. Upscale päälle/pois ei ole live (tekstuurikoot luodaan alustuksessa) — vain
  terävöinti on; siksi kytkintä ei ole valikossa lainkaan.
- Vanha SHM-nimi vaihtui `fo4stereo_shm_v2` → `v3`; molemmat DLL:t käännetty ja asennettu
  (varmuuskopiot `*.bak_mcm`). MCM-tiedostot asennettu `deploy_mcm.ps1`-skriptillä.
- **HUOM: itse MCM-ydin (Nexus mod 21497) ei ole asennettuna** — pelissä on vain MCM Settings Manager
  (56195), joka vaatii ytimen toimiakseen. Käyttäjän pitää asentaa MCM 21497 (Vortex tai manuaalisesti),
  ennen kuin valikko näkyy Pause-menussa.

### MCM-korjaukset (14.11.2026)

- **Juurisyy sille, ettei VR-sivun säädöillä ollut vaikutusta:** `WriteStereoSettings` teki kokonaisrakennekopion,
  joka ylikirjoitti `seq`-laskurin nollalla → seq jäi parittomaksi → lukijan seqlock piti lohkoa "kirjoitus kesken"
  ikuisesti ja proxy ei koskaan lukenut sitä. Eristetty testi (`tmp/test_shm/`) vahvisti: kirjoittaja jätti `seq=1`.
  Korjaus: kirjoitus ja luku kenttä kerrallaan (`stereo_shm.h`), testi vahvistaa `seq=2, read ok=1`.
- **Proxy lukee MCM-tiedoston nyt suoraan** (`Data\MCM\Settings\F4Flat2VR.ini`, mtime-portti, `VrApplyMcm`
  Presentissä) — varmempi polku, joka ei ole riippuvainen pluginin shm-puskusta. Molemmat polut soveltavat
  samat arvot.
- **Dominantti silmä (MCM-kokeilu; päätelmä korjattu 15.11):** tuolloin lisättiin vasen/keskellä/oikea-valinta
  ja oletettiin sen olevan sama lähestymistapa kuin CyberpunkVR:ssä. Lähdekoodin tarkistus osoitti tämän
  oletuksen vääräksi: CyberpunkVR siirtää ADS-asetta/käsivarsia tähtäävän silmän mukaan, ei koko kameraa.
  Kokeilu ja sen asetukset on arkistoitu; koko kuvan tuplautumista tämä kameraoffset ei ratkaise.
- MCM-käyttäjäasetukset palautettu järkeviin arvoihin (`fIPDUnits=4.48`, `fConvergeM=1.5`, `bEyeDelay=1`,
  `iDominantEye=2`): edellisen testin lopussa IPD oli jäänyt 0:aan (stereo pois).
- Molemmat DLL:t käännetty ja asennettu, MCM-valikkotiedostot päivitetty peliin.

### Allekirjoitettu tähtäysoffset + oletukset (14.11.2026)

- **`fAimIpdScale` on nyt allekirjoitettu liuku (−2..2)**, joka korvaa dominant-silmä-steppeerin
  (`iDominantEye` poistettu). Arvo on silmän paikka puolen silmäetäisyyden murto-osana tähdätessä:
  `0 = keskellä (mono)`, `+1 = oikean silmän paikka`, `−1 = vasemman silmän paikka`, |arvo| > 1 työntää
  kauemmas. Molemmat kehykset käyttävät samaa offsetia → tähtäin näyttää yhden kuvan; etumerkki ratkaisee
  miltä silmältä näkymä on. Käyttäjän vahvistus: maksimiasento loi väärän kuvan, 0 ei ole vieläkään
  ihanteellinen → nyt voi hienosäätää millä tahansa välillä −2..2. Konvergenssin määrä tähdätessä on
  `|fAimIpdScale| × 0,5 × IPD` ja kääntösidottu siirtymän suuntaan (ei vaihda kehyksestä toiseen).
- **Käyttäjän viritetyt arvot tallennettu oletuksiksi** (`mcm/F4Flat2VR/settings.ini`): IPD 4.48,
  pään kierto 1, head_pos 1, units/m 70, konvergenssi 4.0, eye_delay 1, FSR-sharp 0.55,
  2D-ruutu 3.0 m @ 1.6 m, Pip-Boy 3.0 m @ 2.0 m, ratiot 0.95/0.75, `fAimIpdScale=−1` (käyttäjä
  tähtää vasemmalla silmällä).
- Plugin käännetty ja asennettu (peli kiinni, hash tarkistettu). Proxy muuttumaton.
- **FRIK (Fallout-4-VR-Body): GPL-3.0 → ei koodia, vain ideat** (projekti on MIT): aseiden
  uudelleensijoitus-offsetit, kamera/keho-kohdistus, fyysinen Pip-Boy, asetusten hot-reload.
  Koodin porttaisi GPL:n alle — viitataan README:n credits-taulukossa ("No code copied").

### Aim-korjaukset: vain 1. persoona + mono ilman toe-iniä (14.11.2026, toinen kierros)

- Käyttäjätesti: sliderin muutos ei näyttänyt vaikuttavan; ase näkyy normaalisti kun sitä katsoo,
  mutta tähdätessä väärin — sekä 3. että 1. persoonassa. Loki vahvisti: `aim 1 (zoom=1) ipdScale=-0.4`,
  eli tunnistus ja sovellus toimivat, mutta **aim-logiikka osui myös 3. persoonaan** (tila 7, joka jakaa
  vtablet tilan 8 kanssa → maskiin kuuluu) ja **toe-in-yaw kiersi mono-näkymää** (identtiset kehykset
  → koko kuva ja ristikko kääntyi luodinradasta).
- Korjaus: aim-mono-offset vain `camState == 0`; 3. persoonan tähtäys käyttää tavallista stereoa.
  Konvergenssin toe-in vain vuorotteleville stereo-kehyksille — mono-tähtäyksessä yaw = 0.
- Odottaa käyttäjätestiä: tähtäys 1. persoonassa arvoilla 0/−1/+1 (ja ääriarvot ±2, koska pienet
  arvot ≈ 1 cm siirtymä eivät juuri näy) ja 3. persoonan tähtäys. Mahdolliset jatkot käyttäjän
  ehdotuksista: per-silmä asepiirto (piilota ase toisen silmän kehyksestä — klassinen S3D-keino,
  mahdollinen koska plugin tietää silmän per kehys) tai aseen/etureisien siirto eteenpäin
  (FRIK-tyylinen weapon offset, idea vain — FRIK on GPL).

### Silmävalinnan korjaus: vuorottelu irti present-pariteetista (14.11.2026, kolmas kierros)

- Käyttäjäraportti: kamera heiluu vasemmalla-oikealla koko ajan; tähtäyksessä ei eroa eikä tuplakuva
  poistunut. Juurisyy löytyi laskureista: **silmä valittiin present-laskurin pariteetilla**
  (`presents & 1`), mutta render-säie ja pelisäie kilpailevat — present voi osua ennen tai jälkeen
  seuraavan kamerapäivityksen, jolloin silmä **toistuu** (sama silmänäkymä kahdesti, toisen silmän
  tekstuuri jää vanhaksi) → vakio vasemmalla-oikealla-heilunta. Mittaus: in-game presents/update ≈ 1:1
  mutta jitteraen; paussit (MCM-valikko) siirtävät pariteettifaasia satunnaiseen kohtaan.
- **Korjaus:** plugin vuorottelee silmän itse **kamera-päivitysten tahdissa** (`eye ^= 1` per
  HookedStateUpdate — sama muuttuja jolla kameransiirto tehdään → täydellinen lukitus), ja proxy
  seuraa kirjattua silmää seq-historian kautta (`s_eyePrev/s_eyeCur`, eye_delay = päivitysviive:
  0 = uusin päivitys, 1 = edellinen). Paussifaasioitsuma ja jatkuva huojunta poistuvat kokonaan;
  mahdollinen jäljelle jäävä virhe on korjattavissa swap_eyes/eye_delay-kytkimillä.
- Molemmat DLL:t käännetty ja asennettu (peli kiinni, hashit). Käyttäjän swap_eyes palautettu 0:aan
  (uusi kaava muuttaa kalibroinnin); eye_delay=1 pidettiin (render-säie jäljessä ~1 päivitys).
- Seuraava askel jos tuplakuva tähtäyksessä jää: per-silmän bounds-rajauksen yhdistäminen mono-tähdätessä
  (kommentti koodissa: "mono aim must collapse them to the same window") tai päänseurannan vaimennus
  tähtäyksen aikana (moottorin oma ADS-kohdistus rikkoutuu head-pos/rot-offseteista).

### Asetusten ylityskorjaus + mono-tähtäyksen rajaus (14.11.2026, neljäs kierros)

- Käyttäjäraportti: kamera heiluu yhä kuin ensimmäisessä prototyypissä (pääosin paikallaan ollessa),
  tuplakuva tähtäyksessä jää, vasemmanpuoleisesta kopiosta puuttui aseen etupään osa, slider ei
  näytä vaikuttavan tähtäyksessä.
- **Loki selitti paljon**: plugin-loki pelin aikana näyttää `aim 1 ... ipdScale=0` ja enginen
  tilannerivin `convergence_m=0 head_rot_compensate=false` — vaikka MCM-lukija oli käynnistyksessä
  lukenut `aim_ipd_scale 0 -> 2` ja `convergence_m 1.5 -> 7.5`. Eli **LoadEngineConfig()**
  (kutsutaan hook-asennuksessa pelin latauduttua) luki fo4stereo.inin ja **ylsitti MCM-lukijan jo
  asettamat arvot** takaisin ini-oletuksiin. MCM-pollisilmukka kirjoittaa vain ini-muutoksilla,
  joten ylitys jäi voimaan koko session ajaksi. Tämä selitti kolme vanhaa palautettaa kerralla:
  - konvergenssi "ei toimi / ei eroa" → runtime-arvo oli 0,
  - aim-slider "ei näy vaikuttavan" → runtime-arvo oli 0,
  - mahdollinen paikallaanheilunta: head_rot_compensate oli runtimessa false (palautettaessä
    true se kompensoi kiertopalautetta, joka vahvistaa pään mikrolikat paikallaan).
- **Korjaus 1 (plugin):** hook-asennuksessa LoadEngineConfigin jälkeen `g_mcmHave=false; ApplyMcm();`
  → MCM-arvot pakotetaan takaisin pelin latauduttua (lokiin tulee mcm-rivit jos arvot erosivat).
- **Korjaus 2 (proxy):** mono-tähtäyksen bounds-yhdistys. Lokin bounds-rivit vahvistivat
  per-silmän rajauksen eron: eye 0 u=0.000..0.821, eye 1 u=0.179..1.000 (~18 % ikkunaeiro);
  sama kuva kahdella eri rajauksella = tuplakuva ja katkennut aseen etupää. Kun zoom==1 ja
  camState==0 (mono-tähtäys, `g_monoAim`), VrEyeBounds käyttää molemmilla silmillä **keskiarvo-
  ikkunaa** (pään keskiö), jolloin molemmat silmät näkevät identtisen kuvan.
- Rakennettu ja asennettu (peli kiinni): plugin hash 527CC186..., proxy hash 828AEEBF...
- Huomio logista: proxy-näytteistysrivi näytti eye=1 vakiona — selittyi näytteistyksellä (joka 600.
  frame, seq edistyy täsmälleen 600/600 → pariteetti aina sama); vuorottelu itsessään todettiin
  toimivaksi (getTrans-loki: eye vuorottelee 0/1 per päivitys, updates/presents 1:1).

### Oletusten palautus + pelikansion siivous (14.11.2026, viides kierros)

- Käyttäjäraportti: huonompi kuin koskaan. Heilunta jatkui; **normaali 1. persoonan kuva (ilman
  tähtäystä) näytti kahtena, ase mukaan lukien**. Pyysi: palauta oletusasetukset (Pip-Boy-arvot
  jäävät), ja varmista ettei pelikansioon ole jäänyt ongelmallisia tiedostoja.
- Analyysi: edellisen kierroksen korjaus aktivoi ensimmäistä kertaa **todellisuudessa** arvot, jotka
  olivat olleet kuolleet (stomp-bugin takia): konvergenssi 7.5 ja rotComp=true. Kaikki käyttäjän
  aiemmat testit ja "parhaat" tulokset oli saatu tilassa, jossa nämä olivat 0/false — joten akti-
  voituessaan ne muuttivat kokemusta, eivät parantaneet sitä. Lisäksi bEyeDelay=1 oli viritetty
  VANHAN kaavan semantiikalla (present-lag); uudessa seq-historia-kaavassa se voi tarkoittaa
  systemaattista silmäpariteetin vaihtoa → käänteinen stereoparallaksi → tuplakuva koko näkymässä.
- **Toimenpiteet:**
  - Käyttäjän MCM-ini palautettu oletuksiin: IPD 4.48, headRot 1, rotComp 0, headPos 1, units 70,
    aimIpdScale -1, **convergence 0** (isoitu pois; kokeillaan uudestaan kun fuusio kunossa),
    **eyeDelay 0** (uuden kaavan semantiikka: 0 = uusin päivitys; A/B-testi MCM:llä),
    swap 0. Pip-Boy (3.0 m @ 2.0 m) säilytettiin.
  - Projektin settings.ini päivitetty vastaamaan (uusi oletuspohja).
  - Pelikansio siivottu: kaikki .bak-tiedostot (12 kpl d3d11/fo4stereo-varmuuskopioita) ja
    virheelliset kopiot (Data\F4SE\fo4flat_probe.dll + 4 vanhaa .bakia, Plugins\*.bak_mcm)
    siirretty tmp\game_backups\ (ei poistettu). Jäljellä: d3d11.dll (proxy), fo4stereo.ini,
    lokit, Data\F4SE\Plugins\fo4flat_probe.dll (ainoa plugin-kopio), mcm.dll, version-bin.
    dxgi.dll ei ole olemassa (ei konfliktia).
- **Testi-ohje käyttäjälle:** jos normaalinäkymä fuusioituu (ei tuplakuvaa) ja heilunta poistuu →
  valmista; jos tuplakuva jää → käännä MCM:ssä "Silmäviive" (bEyeDelay) 0↔1 — vain kaksi
  eri pariteettiparia on olemassa, joten jompikumpi kahdesta asennosta fuusioituu.

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
  kirjoitetaan jaettuun muistiin (`fo4stereo/src/stereo_shm.h`, `Local\fo4stereo_shm_v1`). Proxy lataa
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
  - Jaettu muisti v2 (`Local\fo4stereo_shm_v2`): eye, seq, presents, flatMode, HMD-asento seqlockilla.
  - **Proxy omistaa silmän:** kehyksen silmä = (Present-määrä ennen tätä − eye_delay) & 1. Plugin lukee saman
    laskurin state-Updatessa.
  - **Kompensointi:** GetTranslation palauttaa `orig − prevApplied + curApplied` (`[engine] compensate=1`).
  - **2D-ruutu:** jos kameran päivityksiä (seq) ei tule 20 kehykseen (intro, päävalikko, lataus), proxy näyttää
    kuvan IVROverlay-ruudulla (3 m leveä, 2 m edessä istuma-avaruudessa) ja lähettää silmille mustaa.
  - **Pään seuranta:** proxy kirjoittaa WaitGetPoses-asennon (HMD, istuma-avaruus). Plugin koukuttaa kameratilan
    GetRotationin (0x0C) ja yhdistää pään kierron (VR → peli: q = (w, x, −z, y), `head_rotation` 1/2,
    `head_rot_compensate`). Pään paikka lisätään siirtoon (`head_position`, `units_per_meter=70`).
  - Varmuuskopiot: `d3d11_vr_v1.dll.bak`, `Data\F4SE\fo4flat_probe_stereo1.dll.bak`, `fo4stereo.ini.bak_v1`,
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
  `Data\F4SE\fo4flat_probe_v3.dll.bak`, `fo4stereo.ini.bak_v4`.
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
## New fixes (13.11.2026)

- Mouse capture (ClipCursor) removed from proxy so Alt-Tab to desktop works.
- pipboy_ratio 0.5 -> 0.75, aim_ratio 0.98 -> 0.95, aim_ipd_scale=0 (mono).
- Instrumentation added: proxy logs vr: eye N bounds..., plugin logs aim{} conv yaw....
- Build & deploy: d3d11.dll and fo4flat_probe.dll deployed.


## New fixes (15.11.2026)

- Aiming double vision remains unresolved. The mono-aim path selects one captured eye texture for both eye submissions when available, but this did not prove what the compositor displayed and the user still reported both weapon and background doubled. The new transition logs record each actual `Submit` call separately; compare those with hipfire before changing camera math.
- Camera looks down-left: likely cause is OpenVR seated zero pose, which resets position and yaw but NOT pitch. Added [vr] `head_yaw_offset_deg`, `head_pitch_offset_deg` and `recenter_key` (default F9=120) to fo4stereo.ini. Recenter captures the current head yaw+pitch as offsets, applied to the pose written to shm and to the 2D screen transform (screen follows the new zero pose).
- Plugin log spam: conv yaw logged every frame because the sign alternates per eye; now logs on magnitude change only.
- Both binaries rebuilt and deployed to the game directory.

## Double rendering analysis (15.11.2026)

Measured from the plugin log (03:14:49-03:14:54, 5 s window):

- getTrans calls: 600 -> 120/s
- state updates (HookedStateUpdate): 300 -> 60/s
- presents: 300 -> 60/s

Interpretation: the engine calls getTrans TWICE per present, while the camera state
updates once per game tick. The current eye alternation is per TICK, so the per-eye
rate is present_rate / 2 = 30 Hz with the engine's 60 fps cap.

Options for double rendering (higher per-eye rate / both eyes per frame):

1. **Unlock the engine cap** (iPresentInterval=0): if presents rise above 60, per-eye
   rate rises proportionally IF alternation follows the renders. Risk: alternation is
   per tick, so presents between ticks repeat an eye (L,L,R,R) - new content per eye
   stays at tick rate. Instrumentation needed: what hook runs once per RENDERED frame?
2. **Per-render eye alternation**: if a hook that runs once per render (getTrans is 2x,
   so not 1:1) can apply the eye offset, alternation could follow renders instead of
   ticks. Requires identifying the true per-render camera read on the render thread.
3. **Proxy-side reprojection**: render the game frame, capture the depth buffer, and
   reproject it for the other eye. No engine changes, but GPU cost and edge artifacts.

First experiment: unlock iPresentInterval and watch whether presents in the getTrans
log lines exceeds 60/s and whether getTrans stays at 2x (structural) or becomes 1:1.

## Aim / camera investigation (15.11.2026)

How the engine changes the camera when aiming (ADS), from code + logs:

- FO4 does NOT switch camera states on ADS: the IronSights state (id 4) exists in
  the CameraStates enum, but the logs show camState stays 0 in first person.
- ADS is a FOV zoom only: the weapon's AimModel narrows the projection. The proxy
  measures game tan 1.307 vs normal 1.377 -> ratio 0.950 (about a 5% zoom).
- The camera position/rotation itself is not offset by the engine on ADS.

Consequence: aim detection was a present-time FOV-ratio heuristic with a one-frame
round-trip (plugin reads shm->zoom written by the proxy at the last Present), and
heavy scope zoom would cross pipboy_ratio (0.75) and flip the view to the 2D screen.

New implementation:
- Plugin reads PlayerCamera::fovAdjustCurrent + pipboyMode directly (g_cam is
  already hooked) -> per-tick, zero-lag aim detection (aimFov = fovAdj < 0.999).
- New shm field aim (was pad2): plugin publishes the engine aim flag.
- Proxy: pluginAim overrides the ratio heuristic for zoom=1 (first person only);
  the Pip-Boy projected-target check still wins, so the Pip-Boy stays on the 2D
  screen while scopes stay in VR mono aim.
- MCM settings.ini aligned with fo4stereo.ini (ini is the source of truth):
  fAimIpdScale=0, fConvergeM=1.5, bEyeDelay=1, fUpSharp=0.1, fScreenDist=2.0,
  fPipWidth=0.8.
- Both binaries rebuilt and deployed; FOMOD zip rebuilt (0.2).

---

## Oppianalyysi: CyberpunkVR Port + UEVR + Oblivion UEVR Profile (15.11.2026)

---

### Yhdistetty suunnitelma: Mitä lainaamme kummastakin

| Alue | CyberpunkVR | UEVR / Oblivion Profiili | Meidän toimitehdas |
|------|-------------|--------------------------|-------------------|
| **Stereo-tilat** | Engine-native stereo + VRCAM fallback | Native / Sequential / AFR (valittava live) | **Lisää Synchronized Sequential** (kaksi framea samalla tickillä) + AFR + valinta in-game GUI:ssa |
| **Head tracking** | OpenXR, runtime IPD, world scale | UEVR abstraktoi OpenVR/OpenXR | **Runtime IPD** (OpenVR `Prop_UserIpdMeters_Float`), **World Scale** (yhden skaalauksen kerroin) |
| **Mono-aim** | EngineStereo mono = pään keskipiste | 3DOF motion controls | **Mono-aim = pään keskipiste**, dominant-silmä slider hienosäätö |
| **UI-käsittely** | Native VR-HUD paneelit | Automaattinen UI-projektointi 3D:een + UI Follow View | **Pip-Boy/terminat/container UI → 3D-projektointi** mukavaan syvyyteen, UI Follow View -tila |
| **In-game säädöt** | Debug-exportit (`CyberpunkVR_Debug*`) | ImGui-overlay (Insert/L3+R3), RT+stick shortcuts | **ImGui-overlay Insert-näppäimellä**: live IPD, convergence, camera offset, recenter, stereo mode, standing origin |
| **Motion controls** | Ei (vain HMD) | Fyysinen combat, holsterit, gesture-hyökkäykset, asepyörä | **FRIK-integraatio**: collision-melee, block/parry, holster-järjestelmä, kaksinkertaiset aseet, favoritipyörä |
| **Profiilit** | INI + MCM | Zip-profiilit (config + Lua + ESP) | **Profiilijärjestelmä (JSON)**: export/import, per-ase/per-tilanne, community-jakelu |
| **Injection/Launcher** | Ei (RED4ext) | UEVRInjector.exe (pre-flight checks, VR-plugin nullification) | **Launcher/Injector**: konfliktitarkistus, DLL-backup, uninstall-palautus, admin-check |
| **GPU-optimoinnit** | Piilomaski, FFR/VRS, jaetut varjot, exposure ympäristöstä | Skip Draw / Skip Tick variantit | **Piilomaski linssireunoille**, **VRS/FFR**, **per-silmä depth submit**, **exposure ympäristöstä** |
| **Debug/Telemetria** | `CyberpunkVR_Debug*` globaalit | UEVR Debug-loki | **`fo4flat_Debug*` exportit** + per-frame loki toggle |
---

## Prioriteettilista (Päivitetty 15.11.2026)

### Vaihe 1: Core UX & Rendering (Viikko 1-2) — **KORKOINEN VAIKUTUS**
| # | Ominaisuus | Lähde | Vaikeus | Vaikutus |
|---|------------|-------|---------|----------|
| 1 | **Synchronized Sequential -tila** | UEVR | Korkea | ⭐⭐⭐⭐⭐ — Korjaa tuplakuvan ilman proxy-hakkia, game time ei advance välissä |
| 2 | **In-game ImGui-overlay (Insert-näppäin)** | UEVR | Keskitaso | ⭐⭐⭐⭐⭐ — Live IPD, convergence, camera offset, recenter, stereo mode, standing origin |
| 3 | **Runtime IPD OpenVR:stä** | CyberpunkVR | Matala | ⭐⭐⭐⭐ — Nollaa kalibroinnin, toimii joka käyttäjälle |
| 4 | **World Scale -kerroin** | CyberpunkVR | Matala | ⭐⭐⭐⭐ — Yhden sliderin "maailman koko" (IPD + head pos) |
| 5 | **Standing Origin -järjestelmä** | UEVR | Keskitaso | ⭐⭐⭐⭐ — Erottaa lattiakalibraation kamera-fine-tuningista |
| 6 | **Mono-aim = pään keskipiste** | CyberpunkVR | Matala | ⭐⭐⭐⭐ — Luonnollisempi tähtäys, vähemmän silmäyliuistelu |

### Vaihe 2: Motion Controls & Combat (Viikko 3-4) — **FRIK-INTEGRAATIO**
| # | Ominaisuus | Lähde | Vaikeus | Vaikutus |
|---|------------|-------|---------|----------|
| 7 | **Collision-pohjainen melee** | Oblivion UEVR | Korkea | ⭐⭐⭐⭐⭐ — Lyönnin voima → damage tier, shield-block collision-normaalilla |
| 8 | **Holster-järjestelmä (body-relative)** | Oblivion UEVR | Keskitaso | ⭐⭐⭐⭐ — Quickslotit ruumiin paikkoihin (olkapää, vyötärö, rintakehä) |
| 9 | **Kaksoiskäsivyöt / kaksinkertaiset aseet** | Oblivion UEVR | Keskitaso | ⭐⭐⭐ — Molemmat controllerit samalla objektilla |
| 10 | **Gesture-hyökkäykset + block/parry** | Oblivion UEVR | Korkea | ⭐⭐⭐⭐ — Swipe force → light/heavy; kilven kääntäminen vihun aseen suuntaan = parry |
| 11 | **Favoritipyörä (Quickslot Wheel)** | Oblivion UEVR | Matala | ⭐⭐⭐ — Grip+tick → radiaalivalikko world-space |
| 12 | **UI Follow View (Pip-Boy/terminat)** | Oblivion UEVR | Keskitaso | ⭐⭐⭐⭐ — UI seuraa päätä, head-aim väliaikaisesti |

### Vaihe 3: Advanced Rendering & Polish (Kuukausi 2) — **LAADUN NOSTAMINEN**
| # | Ominaisuus | Lähde | Vaikeus | Vaikutus |
|---|------------|-------|---------|----------|
| 13 | **Automaattinen UI-projektointi 3D:een** | UEVR | Korkea | ⭐⭐⭐⭐ — HUD/valikot mukavaan syvyyteen, luettava koko |
| 14 | **Per-silmä depth submit (OpenVR)** | CyberpunkVR | Keskitaso | ⭐⭐⭐⭐ — ATW/reprojektiotehus paranee merkittävästi |
| 15 | **Piilomaski linssireunoille + VRS/FFR** | CyberpunkVR | Keskitaso | ⭐⭐⭐ — GPU-säästö resoluutiorajoitteisessa |
| 16 | **Profiilijärjestelmä (JSON export/import)** | UEVR | Keskitaso | ⭐⭐⭐ — Per-ase/per-tilanne, community-jakelu |
| 17 | **Launcher/Injector pre-flight checks** | UEVR | Keskitaso | ⭐⭐⭐ — Konfliktitarkistus, DLL-backup, uninstall-palautus |
| 18 | **Asymmetrinen projektio per silmä** | CyberpunkVR | Korkea | ⭐⭐⭐ — Linssikeskuksen mukainen frustum, vähemmän distorsiota |
| 19 | **OSR (One-Side Reconstruction) tila** | CyberpunkVR | Korkea | ⭐⭐⭐⭐ — "Melkein natiivi" framerate, toinen silmä rekonstruoituna |
Tässä päivitetään sisäinen suunnittelu dokumentaatio kattamaan molemmat viiteprojektit:
[CyberpunkVR Port](https://github.com/dariulone/cyberpunk-vr-port) (RED4ext, OpenXR, engine-native stereo)
### A. CyberpunkVR Port — RED4ext / OpenXR / Engine-native stereo

**Perusperiaatteet (CameraMath.cpp + EngineStereo.cpp + SyncStereo.cpp):**
- **Aito stereo, ei uudelleenprojisointia:** toinen silmä on oma moottorin render-kamera.
- **AFW (Alternate Frame Warping):** silmät vuorottelevat kehyksissä, toinen silmä rekonstruoidaan edellisen kehyksen perusteella.
- **Asymmetrinen projektio per silmä:** jokaisen silmän näkymä rajataan headsetin linssikentän mukaiseksi.
- **OpenXR head tracking:** HMD-asento injektoidaan render-polkuihin, per-eye depth-kerrokset parantavat reprojektiota.
- **Natiivinen VR-HUD:** HUD-elementit omille paneeleilleen, free-look-kartiot säädettävissä.
- **FSR1 upscale:** ei-temporaalinen, VR-turvallinen.
---

## Tekniset velkakirjat — Miten nämä ratkaisut auttavat

| Ongelma | Nykyinen tilanne | Ratkaisu uudella suunnitelmalla |
|---------|------------------|--------------------------------|
| **Tuplakuva ADS:llä liikuttaessa** | Proxy kopioi uusimman framen molempiin silmiin (hakk) | Synchronized Sequential: molemmat silmät SAMALLA tickillä → nollaa temporaaliset erot |
| **Kamera "down-left" (pitch ei resettoidu)** | F9 recenter (F9 = quickload konfliktissa) | Standing Origin + Delete-näppäin recenter (yaw+pitch), Home = standing origin reset |
| **Hiiren scroll vie sivulle** | Head position lasketaan kameran akselilla, ei pään | Head position rotattuu pään orientaatiolla (oikein toteutettu 15.11) |
| **MCM ei näy / ei toimi** | Config-kansio puuttui, MCM.ini ei syntynyt | Deploy-skripti korjattu, profiilijärjestelmä varmistaa asetusten tallennuksen |
| **Puolet frameraatesta (AFR)** | Silmät vuorottelevat kehyksissä | Synchronized Sequential (kaksi framea samalla tickillä) TAI OSR (rekonstruktio) |
| **Konvergenssi kääntää mono-näkymän** | Toe-in yaw mono-tilassa | Mono-tilassa yaw = 0 (vain vuorotteleva stereo saa convergence) |
| **IPD kovakoodattu** | `ipd_units=4.48` INI:ssä | Runtime IPD OpenVR:stä + World Scale -kerroin |
| **Ei motion controls** | Pelkkä gamepad | FRIK-integraatio: collision-melee, holsterit, gesture-hyökkäykset |

---

## Arkkitehtuurin muutos: Modulaarinen Core

```
f4flat2vr/
├── core/                    # Yhteinen logiikka (kamera, HMD, stereo-math)
│   ├── CameraMath.hpp       # CyberpunkVR: FOV, IPD, basis-rakennus, world scale
│   ├── PoseIdentity.hpp     # CyberpunkVR/UEVR: pose-fingerprinting, staleness detection
│   ├── StereoMode.hpp       # UEVR: Native / Sequential / AFR / OSR enum + tilakone
│   ├── WorldScale.hpp       # CyberpunkVR: yhtenäinen skaalaus IPD + head pos
│   └── UIProjection.hpp     # UEVR: UI 3D-projektointi, Follow View
├── plugin/                  # F4SE-plugin (moottorikamera, head tracking)
│   ├── CameraHooks.cpp      # GetTranslation/GetRotation hookit
│   ├── StereoController.cpp # Silmävuorottelu, offset-lasku, stereo-mode logiikka
│   ├── HeadTracker.cpp      # OpenVR pose → moottorioordinaatisto
│   ├── MotionController.cpp # FRIK: collision-melee, holsterit, gestureraw
│   └── ProfileManager.cpp   # JSON profiilien luku/kirjoitus
├── proxy/                   # D3D11-proxy (frame capture, OpenVR submit)
│   ├── FrameCapture.cpp     # Present-hook, eye texture management
│   ├── StereoSubmit.cpp     # OpenVR submit, per-silmä depth, AFW/OSR/Sequential
│   ├── ImGuiOverlay.cpp     # UEVR: Insert-näppäin → live-säädöt
│   ├── Reprojection.cpp     # CyberpunkVR: OSR depth+motion vector rekonstruktio
│   └── HiddenAreaMask.cpp   # CyberpunkVR: linssipiilomaski
├── launcher/                # UEVR: UEVRInjector.exe kaltainen
│   ├── PreflightChecks.cpp  # Konfliktit, admin, overlayt, DLSS FG, GPU scheduling
│   ├── DllBackup.cpp        # Varmuuskopioi DLL:t, uninstall-palautus
│   └── ProfileDeploy.cpp    # JSON-profiilien asennus/päivitys
└── mcm/                     # MCM-valikko (olemassa, säilyy)
```
## Toteutus: Runtime IPD + World Scale (15.11.2026)

Suunnitelman Vaihe 1:n matalan riskin korkean vaikutuksen kohteet toteutettu.

### Runtime IPD (OpenVR)
- **Proxy** (`VrQueryHmdView`): lukee `Prop_UserIpdMeters_Float` IVRSystemistä, kirjoittaa
  jaettuun muistiin (`shm->runtimeIpdM` + `runtimeIpdValid`). Loki: `vr: runtime IPD 0.0640 m`.
- **Plugin** (`HookedStateUpdate`): `g_effIpd` lasketaan per kehys:
  `baseIpd = runtimeIpdM * units_per_meter` (auto) tai `ipd_units` (käsin), sitten `* world_scale`.
  Silmäsiirto, aim-offset ja konvergenssi käyttävät `g_effIpd`.
- **INI:** `[engine] use_runtime_ipd=1` (oletus). `0` = käytä `ipd_units`.

### World Scale
- Yksi kerroin (`[engine] world_scale`, oletus 1.0) kertoo SEKÄ silmäetäisyyden ETTÄ pään
  siirtogainin (`s = units_per_meter * world_scale`), joten maailman koko muuttuu yhdellä
  liukusäätimellä ilman että stereo kalibrointi hajoaa.
- **SHM:** `StereoSettings.worldScale` (plugin kirjoittaa, proxy lukee muttei sovella — engine-puoli).
- **MCM:** uusi liuku *Maailman koko (world scale)*, 0.5..2.0, hienosäätö 0.05.
- **INI:** `[engine] world_scale=1.0`.

### Samalla korjatut perusasetukset
- `vr_cam_states=0` (vain 1. pers.) → 3. persoonan tuplakuva poistuu (ehdokas).
- `recenter_key=46` (Delete, ei F9 = pikalahoitus).
- `fo4flat_probe.dll` Skew-fix: `VrQuatToGameQuat` 3 etumerkkiä korjattu.
- `fovAdj <= 0 → 1.0` fallback estää väärän aim-tunnistuksen (`fovAdj=0.000`).
## Nausea root cause + aim detection finding (15.11.2026)

### Nausea = wrong head-rotation basis (FIXED)
- Oire: huono olo heti pelatessa.
- Syy: `VrQuatToGameQuat` (matriisipohjainen similarity-transform) kirjoitti pään
  **yawin pelin eteenpäin-akselille (roll)**. Loki: plugin `head=(0.98 -0.01 0.19 0.03)`
  → y-komponentti dominoi (y = forward = roll). Pään kääntäminen siis keinutti
  koko maailmaa → sekunnissa pahoinvointi.
- Korjaus: palautettu **yksinkertainen oikea kantoformaatiokonjugaatio**:
  `gameQ = (w, vr.x, -vr.z, vr.y)` eli VR pitch→right, roll→forward(-), yaw→up.
  Tämä on matemaattisesti oikea muunnos ortonormaalille, det +1 -kannalle T.

### Aim-tunnistus on kuollut (EI vielä korjattu)
- Plugin: `fovAdj=0.000 target=0.000 perSec=0.000` koko session → `PlayerCamera::fovAdjustCurrent`
  lukee 0 (väärä offset tälle runtime 1.11.240:lle TAI kenttä ei ole se mitä luultiin).
  Fallback (`<=0 → 1.0`) estää väärän triggerin mutta aim ei koskaan laukea.
- Proxy: `game tan x=1.377` luetaan gameplayn aikana, mutta ADS:n FOV-kavennus on vain ~5 %
  (tan 1.377→~1.307, ratio ~0.95) eikä ylitä `aim_ratio=0.95`-kynnystä → `zoom` pysyy 0.
  Lisäksi mitattu tan heiluu 1.349..1.400 normaaliajossa.
- Seuraus: `aim_ipd_scale` ei koskaan aktivoidu → ADS:ssä täysi stereo → tuplakuva tähtäimessä.

**Ratkaisu vaatii oikean datan:** RenderDoc-kaappaus ADS:stä paljastaa (a) pelin oikean
kameran projektiokonstantit ja FOV-arvot ja (b) `PlayerCamera`-rakenteen oikean kentän
## Head tracking fix + live ADS calibration (15.11.2026)

- **Päänseuranta toimi vihdoin:** juurisyy oli MCM `bHeadRotComp=1` → plugin laski
  `prev⁻¹·current` = per-kehyksen delta ≈ identiteetti, joten pään kääntö ei liikuttanut
  kameraa (vain hiiri). Asetettu 0:aan (MCM Settings + fo4stereo.ini). Samalla nollattu
  kokeilussa karanneet arvot: `fIPDUnits 10→4.48`, `fAimIpdScale −2→−0.5`.
  Käyttäjä vahvisti: pään kääntö kääntää näkymää, tähtäys seuraa katsetta, pahoinvointi poissa.
- **Live ADS-kalibrointi (uusi):** pluginissa VK_LEFT/VK_RIGHT (nuolet) säätävät
  `aim_ipd_scale`:a 0.05 askelin (edge-trigger + autorepeat 120 ms), clamp −2..2.
  Siirtää mono-tähtäyskuvaa silmän suuntaan → kohdistaa jyväisen ja poistaa jäännöskaksoiskuvan.
  Loki: `aim calibrate: aim_ipd_scale=X.XX`. Lopullinen arvo tarkoitus ajaa MCM/iniin.
- Aseen/etureisien fyysinen kääntyminen katseen mukaan = body-IK (FRIK-luokka, GPL → ei koodia);
  tulevaisuuden vaihe. Ensimmäisen persoonan ase seuraa jo kameraa → tähtäys on katseessa.

zoom-signaalille. Ilman tätä arvaaminen on hakuammuntaa.

**Väliaikainen kokeilu:** `[vr] aim_ratio=0.98` (tai 0.97) saattaisi laukaista ADS:n ~5 %
kavennuksella — testattava, mutta riski väärästä triggeristä normaalin FOV-heilunnan takia.

Molemmat DLL:t käännetty ja asennettu 15.11.2026 (plugin 316 KB, proxy 272 KB).

**Testattava:** käynnistä peli, tarkista lokista `vr: runtime IPD ...` ja että pään
kääntäminen kääntää 3D-näkymää (ei kuvalaatikkoa), eikä peruspelissä ole tuplakuvaa
1. tai 3. persoonassa. Tämän jälkeen säädetään aseen tähtäys.

**Kriittisiä sovellettavia oppia:**

| Oppi | Sovellus meidän projektiin |
|------|---------------------------|
---

## Seuraavat askeleet (Immediate Action Items)

1. **TÄMÄ VIIKKO:** Luo `core/StereoMode.hpp` + pluginin `StereoController.cpp` Synchronized Sequential -tilalle (kaksi framea samalla engine-tikillä).
2. **TÄMÄ VIIKKO:** Lisää `proxy/ImGuiOverlay.cpp` Insert-näppäiminen ImGui-overlay (IPD, convergence, camera offset, recenter, stereo mode, standing origin).
3. **TÄMÄ VIIKKO:** Implementoi Runtime IPD (`GetFloatTrackedDeviceProperty(Prop_UserIpdMeters_Float)`) + World Scale -kerroin.
4. **ENSIMMÄINEN TESTI:** Synchronized Sequential + ImGui overlay → ADS + liike → tuplakuvan on poistunut ilman proxy-hakkia.
5. **SEURAAVA VIIKKO:** Standing Origin järjestelmä (F9=recenter yaw+pitch, Home=standing origin reset) + Delete-näppäin recenter.
6. **SEURAAVA VIIKKO:** FRIK-integraation suunnittelu: collision-melee prototyyppi.

---

## Viitteet & Lähteet

- **CyberpunkVR Port:** https://github.com/dariulone/cyberpunk-vr-port
  - `src/Camera/CameraMath.cpp` — FOV, IPD, basis, world scale
  - `src/Stereo/EngineStereo.cpp` — Engine-native stereo, mono mode, convergence
  - `src/Stereo/SyncStereo.cpp` — AFW/OSR, frame timing, frame identity
  - `src/Stereo/Capture.cpp` — Frame capture, RTV management, depth submit
  - `src/Camera/PoseIdentity.cpp` — Pose fingerprinting, staleness detection
  - `include/Stereo/EngineStereo.hpp` — Stereo mode API, mono mode, UI handling

- **UEVR:** https://github.com/praydog/UEVR
  - README — 3 rendering modes, in-game GUI, motion controls, profile system
  - Oblivion Remastered UEVR Profile: https://www.nexusmods.com/oblivionremastered/mods/878
    - Fyysinen combat, holsterit, UI Follow View, asepyörä, standing origin, config UI

- **FRIK (Fallout 4 VR Body):** https://github.com/madjar/FRIK — GPL-3.0, idea-viittaus vain (ei koodikopia)
| **AFW + oma rekonstruktio (OSR):** toinen silmä rakennetaan edellisen kehyksen väristä, syvyydestä ja liikkeestä (~0,44 ms GPU) → kehyksen hinta ~yksi silmä. | Suora vastaus meidän "puolet frameraatesta" -rajoitteeseen. OSR tuottaisi "melkein natiivi" framerate. |
| **Näkymät tunnistetaan nimellä, ei eliminoinnilla:** virtuaalikameran nimen hash. | Proxy tunnistaa kameratilat indeksillä; sama oppi pätee Pip-Boy-kohteen valintaan. |
| **Molempien näkymien pitää jakaa rakenteet:** varjokaskadit, shader-kello, heijastusmarssi. | Meidän silmäsiirto moottorin omassa kamerassa välttää ongelman. |
| **Upskalerille oma konteksti per silmä:** FSR2/DLSS keräävät toisen silmän historiaa → IPD:aave. | FSR1 on turvallinen; jos siirrytään FSR2/DLSS:ään, konteksti pitää jakaa per silmä. |
| **Per-silmä syvyyskerros submitissä:** `IVRCompositor::SubmitWithLayer` + depth. | Me OpenVR:ssä: SubmitWithLayer + depth-kerros parantaa ATW:tä merkittävästi. |
| **Runtime IPD:** `OpenXRManager::GetRuntimeIpd()` → automaattinen kalibraatio. | OpenVR:stä `GetFloatTrackedDeviceProperty(Prop_UserIpdMeters_Float)` poistaisi kalibroinnin. |
| **World Scale:** `xr_world_scale` kertoo SILMÄETÄISYYDEN JA PAÄN SIIRTOGAININ yhdellä liukusäätimellä. | Yhdistä `units_per_meter` + `ipd_units` → `world_scale` kerroin. |
| **Vertikaalinen FOV totuuslähteenä:** moottorin FOV on VERTikaalinen. | Proxy mittaa game tan X/Y; varmista vertikaalinen FOV perusta. |
| **Mono-tila = pään keskipiste:** EngineStereo mono = yksi kuva pään keskipisteestä molemmille silmille. | `aim_ipd_scale=0` tekee kameran offsetista nollan; ADS-monon toteutuminen on erikseen varmistettava. CyberpunkVR:n ADS-kohdistus puolestaan muuttaa aseen poseerausta silmän mukaan. |
| **Komprehensiiviset debug-exportit:** `CyberpunkVR_Debug*` globaalit muuttujat. | Lisää `fo4flat_Debug*` exportit plugin/proxy:lle. |
ja [UEVR](https://github.com/praydog/UEVR) + [Oblivion Remastered UEVR-profiili](https://www.nexusmods.com/oblivionremastered/mods/878) (Lua-scriptattu fyysinen combat, holsterit, UI Follow View).
### B. UEVR — Universaali UE4/5 VR-injektori + Oblivion Remastered UEVR-profiili

**Arkkitehtuuri (README + UEVR-profiilin ominaisuudet):**
- **Prosessin injektori (UEVRInjector.exe):** .NET 6.0 GUI, runtime-valinta (OpenVR/OpenXR), VR-plugineiden nullifikaatio.
- **Kolme renderöintitilaa (valittava live):**
  1. **Native Stereo** — Engine oma stereo-pipeline (paras laatu/perf)
  2. **Synchronized Sequential** — Kaksi framea SAMALLA engine-tikillä, synkronoitu (korjaa bugeja, TAA-ghosting)
  3. **AFR** — Silmät vuorottelevat kehyksissä (nausea-riski)
- **In-game GUI (Insert / L3+R3):** ImGui-overlay, live-säädöt:
  - RT + Left Stick: Kamera-offset
  - RT + Right Stick: Kamera-offset ylä/ala
  - RT + B: Reset camera offset
  - RT + Y: Recenter view
  - RT + X: Reset standing origin
- **Automaattinen UI-projektointi 3D-avaruuteen:** UI render-passit havaitaan, projisoidaan mukavaan syvyyteen.
- **Motion Controls Blueprint/Lua-scriptattavissa:** 3DOF/6DOF, roomscale movement.
- **Profiilijärjestelmä:** Profiilit = zip-paketit (config + Lua + ESP/ESM + per-exe targeting).

**Oblivion Remastered UEVR-profiilin (Pande4360) konkreettisia innovaatioita:**

| Ominaisuus | Teknistys | Sovellus Fallout 4:een |
|-----------|-----------|------------------------|
| **Fyysinen combat (collision-pohjainen):** aseen/kielen törmäys → vahinko, voima = kevy/raske, parry = kielen/aseen törmäys vihun aseeseen. | Lua-scripti hookaa fysiikkamoottorin collision-eventit. | Havok-fysiikka → FRIK-integraation kautta collision-melee, block, parry. |
| **Holster-järjestelmä (body-relative):** Quickslotit kartoitetaan ruumiin paikkoihin. | Quickslot-mappaus body-local koordinaatteihin. | Pip-Boy quickslotit → ruumiin paikkoihin (MCM-sivu "Holster Positions"). |
| **Kaksoiskäsivyöt:** molemmat kädet vyössä → realistinen vetäytys. | Lua tunnistaa molemmat controllerit samalla objektilla. | Kaksinkertaiset aseet → kaksinkertainen käden asento. |
| **Gesture-hyökkäykset:** lyönnin voima → kevy/raske; collision → block/parry. | Controllerin vauhti + suunta + collision-normal. | Melee-aseet: swipe-force → damage tier; shield-block kun kääntää kilven vihun aseen suuntaan. |
| **UI Follow View -tila:** UI seuraa päätä, A-painike välillä vaihtaa head-aimingiin. | Lua muuttaa UI-kameran parentiksi HMD:n. | Pip-Boy/terminat/container UI: UI seuraa päätä, head-aim väliaikaisesti. |
| **Asepyörä (Quickslot Wheel):** LeftGrip+Y → radiaalinen valikko ruumiin paikkoihin. | ImGui/Lua radial menu world-space. | Favoritit/quickslotit: controller-grip + tick → radiaalivalikko. |
| **Config UI UEVR:n sisällä:** Lua-scriptattu ImGui-valikko. | Lua + ImGui = live-säädöt ilman pelin pysäytystä. | Lisää in-game ImGui-overlay (Insert-näppäin) pika-säädöille. |
| **Standing Origin -järjestelmä:** RT+X = nollaa standing origin, RT+B = nollaa kamera-offset, RT+Y = recenter view. | Erottaa "lattiakalibraation" "kamera-fine-tuningista". | Lisää `standing_origin` + `camera_offset` erottelu; F9 recenter → yaw+pitch, Home → standing origin reset. |
| **Pre-injection checklist (UEVRInjector):** HDR pois, admin, -nohmd, VR-pluginit pois, overlayt pois, DLSS FG pois. | Yksi klikkaus "Prepare Game" -toiminto. | Launcher/injector: konfliktitarkistus, DLL-backup, uninstall-palautus. |
| **Per-exe profiling:** Win64 (Steam) vs WinGDK (GamePass) eri profiilit. | Auto-detektio game versionista. | Steam/GOG/GamePass -detektointi profiilien valintaan. |
## 17.11.2026 – HUD-paneelit, säätötila, HUD-tiedoston mittaus ja patchaus

**Tehty (proxy, `d3d11.dll`):**
- Keskustelupaneeli: valinnat leikataan silmätekstuurista (`dialogue_panel_crop`), tausta tummennetaan (`VrDialoguePanelUpdate`). Paneeli pysyy näkyvissä koko keskustelun (`dialogue_panel_gate=0`), koska valintojen tunnistus vilkutti paneelia.
- Oma ristikko: proxyn generoima tekstuuri head-locked-overlayna (`reticle`, `reticle_deg`, `reticle_dist_m`, `reticle_yaw_deg`, `reticle_pitch_deg`). Pelin oma ristikko pois `bCrosshairEnabled=0`.
- HUD-paneelit: HP, ammu ja kompassi leikataan pelin HUD:sta ja näytetään head-locked-paneeleina (`hud_panels`, `hud_*_crop`, `hud_*_yaw/pitch/width`). Alkuperäinen HUD peitetään maailmakuvasta (`hud_mask`). Paneelit lukevat kehyksen, joka on juuri kirjoitettu (`g_hudSrc`), jotta vilkkuminen loppuu. Tausta on läpinäkyvä, jos `hud_bg_alpha=0`.
- Säätötila pelissä: Numpad 5 päälle/pois, Numpad 1/9/3 valinta (HP/ammu/kompassi), 4/6 ja 8/2 siirto, +/- koko, 0 tallennus `fo4stereo.ini`-tiedostoon. Keskellä ruutua näkyvä ohjepaneeli.
- Kolmas persoona: HUD-paneelit näkyvät myös tilassa 7.

**Tehty (HUD-tiedoston mittaus ja patchaus):**
- Pelin oma `HUDMenu.swf` purettiin `Fallout4 - Interface.ba2`-arkistosta (vain luettu). Työkalut: `tools/hud/` (`extract_one.py`, `swf_layout.py`, `swf_patch.py`). Bethesdan tiedostoa ei ole repossa.
- Mittaus (stage 1280×720): `BottomCenterGroup_mc` (640, 684), `CenterGroup_mc` (640, 360), `HUDNotificationsGroup_mc` (64, 36). `HPMeter_mc`, `AmmoCount_mc` ja `CompassWidget_mc` ovat ryhmien sisällä.
- Testi-patch: `BottomCenterGroup` ylös 40 px, `HUDNotificationsGroup` vasemmalle 20 px. Asennettu pelikansioon `Data\Interface\HUDMenu.swf` (alkuperäinen palautetaan poistamalla tiedosto). Compass-leikkausalue siirretty vastaamaan (`hud_compass_crop=0.37,0.85,0.63,0.93`).

**Avoimet:**
- Testi-HUD:n toimivuus pelissä (Flash-lataus, sijainnit, luettavuus).
- Kartta- ja tehtävämarkkerit eivät ole vielä mukana paneeleissa.
- Pään kääntämisen sulavuus ja tuplakuva: rakenteellinen (AFR), ei korjattu.
- MCM-sivu näkyvyydelle ja sijainnille: erillinen mod, ks. `docs/TODO.md`.

## 18.11.2026 – MO2, MCM, HUD-kehys ja HUD-sovitus

**Tehty:**
- Mod Organizer 2 (virallinen v2.5.2) asennettu kannettavana `C:\Modding\MO2`. Instanssi `F4Flat2VR`, mod `F4Flat2VR_Dev` (ei pelikansion kopiointia).
- MCM-konfiguraatio (`mo2/F4Flat2VR_Dev/MCM`): sivut HUD-alue (reunat), Keskustelu ja Yleiset. Proxy lukee MCM:n asetustiedoston, kun se on olemassa.
- HUD-kehys: reunat asteina (vasen/oikea/yläreuna/alareuna). Elementit sijoitetaan kehyksen sisään ja niiden koko rajoitetaan reunoihin. Numpad 5 -säätötila (kehys/elementti/leikkaus), Numpad +/- zoomaa koko HUD:n, Numpad 0 tallentaa reunat.
- Kaatuminen Papyrus-natiivin rekisteröinnissä (`BindNativeMethod`) todennettu myös tyhjällä no-op-funktiolla, joten Papyrus-natiivireitti hylättiin. Plugin palautettu pre-Papyrus-versioon.
- ESP ja Papyrus-skripti (`Fo4StereoHud`) luotiin, mutta niitä ei käytetä ennen kuin Papyrus-reitti on korjattu.
- `tools/hud/swf_fit.py`: sovittaa pelin oman `HUDMenu.swf`:n juuritason sijoitukset kehykseen (tasainen skaalaus). Mittaus osoitti, että peli ei piirrä HUD:ia täysin laskennallisesti, joten sijoitus pitää kalibroida. Kalibrointitesti tehtiin (`BottomCenterGroup` siirretty 500 px ylös).
- Keskustelupaneeli palautettu kehyksen keskelle (`dialogue_panel=1`).

**Avoimet:**
- HUD-sijoituksen kalibrointi (kalibrointitestin tulos ja muunnoksen korjaus).
- Kuvanlaatu: kehyksen pystykorkeus rajoittaa tekstin kokoa.
- Pään kääntämisen sulavuus ja tuplakuva (AFR-rakenne), ei korjattu.
- Papyrus-reitti (HP/ammu) pois, kunnes natiivin rekisteröinti on ratkaistu.
