# HUD-suunnitelma (16.–17.11.2026)

Tavoite: HUD (HP, ammo, kompassi, "Sanctuary"-teksti, dialogi) näkyy kypärässä mukavalla etäisyydellä,
ei pelimaailman parallaksissa eikä reunoilla.

## 1. Mitä VR-tiedostoista saatiin selville

Lähde: `D:\Fallout 4 VR\vr-files` (vain suunnittelun vertailuun, ei kopioida repoon).

| Tiedosto | Koko / rakenne | Merkitys meille |
|---|---|---|
| `Meshes\world_HUD.nif` | 6 KB, yksi NiNode + yksi BSTriShape (nelikulmio) + BSEffectShaderProperty + alpha | VR:n HUD on **yksi tasainen nelikulmio maailmassa**, johon HUD on tekstuurina |
| `Textures\world_HUD.dds` | 1024×1024 DDS, ~1,4 MB | HUD on **yksi tekstuuriatlas**: koko HUD piirretään tähän ja sijoitetaan nelikulmioon |
| `Interface\HUDMenu.swf` | 127 KB, Scaleform (CWS = zlib-pakattu) | VR:n HUD-logiikka ja asettelu (HP-, ammo- ja kompassikentät) |
| `Interface\world_crosshair.swf` | 123 KB | Ristikon oma Scaleform-tekstuuri (ei kopioida) |
| `Interface\DialogueMenu.swf` | 27 KB | Dialogivalikon asettelu VR:ssä |
| `Meshes\world_projectedPipBoyScreen.nif` | 7 KB | Pip-Boyn projisointi (vastaa jo toteutettua projisoitua Pip-Boy-ruutua) |

**Johtopäätös:** VR ei säilytä HUD:ia kuvan osana. Se renderöidään omaan tekstuuriin, ja tekstuuri
sijoitetaan maailmaan. Tämä on sama periaate kuin meidän keskustelupaneelissa ja ADS-overlayssa, joten
arkkitehtuuri on jo olemassa.

## 2. Nykytila flat-pelissä

- HUD piirtyy samaan backbufferiin kuin maailma (ei erillistä kerrosta).
- Silmäkohtainen rajaus (u 0..0,81 / 0,19..1,0) tekee HP:stä vain vasemman silmän ja ammusmäärästä vain oikean.
- Alareunan HUD on lähellä rajaa, joten se jää osittain linssin ulkopuolelle.
- Pelin asetukset: `Fallout4Prefs.ini` → `fHUDOpacity=1.0`, `bCrosshairEnabled=0` (asetettu).
  HUD-värit: `iHUDColorR=18`, `iHUDColorG=255`, `iHUDColorB=21`.

## 3. Vaihtoehdot

| Vaihtoehto | Kuvaus | Hyöty | Riski |
|---|---|---|---|
| A. Piilota pelin HUD (`fHUDOpacity=0`) ja tee oma | Proxy piirtää HUD:n itse | Puhdas, ei päällekkäisyyttä | Arvot (HP, ammo) pitää lukea pelistä |
| B. Siirrä pelin HUD: leikkaa alueet ja näytä omina paneelinaan | Kopioi HUD-alueet kehyksestä, näytä head-locked-paneeleina, maskaa alkuperäinen | Ei tarvitse arvoja | Maskaus on vaikea, reunat voivat näkyä |
| C. VR:n tapa: HUD-atlas tekstuuriksi | Piirrä HUD tekstuuriin ja sijoita maailmaan | Vastaa VR:ää | Vaatii HUD-datan lukemisen, paljon työtä |

**Suositus: B ensin, sitten A.** B tuottaa nopeimmin luettavan HUD:n ilman arvojen lukemista. A on
pitkän aikavälin tavoite, kun arvot löytyvät.

## 4. Vaiheet

**Vaihe 0 – mittaus (ei koodimuutoksia):**
1. Testi: `fHUDOpacity=0` pelin ini:ssä. Otetaan Delete-kuva hipfiressä. Jos HUD häviää, A on mahdollinen.
2. Kartoitetaan HUD-alueet 2433×2524 silmäkuvasta (tallennetut kuvat `fo4stereo_shots`):
   - HP-palkki ja "Sanctuary" ~ vasen alakulma
   - ammu (003/006) ~ oikea alakulma
   - kompassi ~ alakeskusta
   - dialogivalinnat ~ alakeskusta (jo tiedossa: u 0,33–0,67, v 0,79–0,90)

**Vaihe 1 – paneelit (vaihtoehto B, proxy):**
3. Määritellään alueet asetuksina: `hud_hp_crop`, `hud_ammo_crop`, `hud_compass_crop` (u0,v0,u1,v1).
4. Jokaiselle alueelle oma head-locked overlay (kuten `dialogue panel`), sijoitus vasemmalle alas, oikealle alas,
   keskelle alas. Leveys ja etäisyys asetuksina.
5. Alkuperäinen HUD maskataan silmätekstuureista samoilla alueilla (mustaksi tai kevyeksi sumennukseksi).
   Tämä tehdään proxyn kopiovaiheessa `CopyResource`-kutsun jälkeen (ei vaadi uutta shaderia).
6. Testi: kuvat `fo4stereo_shots`-kansiosta (Delete). Tarkista, ettei HUD ole kahdesti eikä reunoilla.

**Vaihe 2 – arvot (vaihtoehto A):**
7. Selvitetään HP ja ammu PlayerCharacter-olion kentistä (RTTI-haku kuten PlayerCamera). Riski: REL-osoitteet
   eivät toimi 1.11.240:ssä, joten käytetään RTTI:tä ja vahvistetaan lokista.
8. Piirretään HUD itse proxyssa (HP-palkki, ammu, kompassi vektorina).

**Vaihe 3 – dialogi ja valikot:**
9. Dialogin valinnat: nykyinen paneeli on jo olemassa. Tarkistetaan sijainti ja koko.
10. Pip-Boy: jo projisoitu, ei muutoksia.

## 5. Riskit

- Maskaus voi jättää tummia laikkuja maisemaan. Sumennus on siistimpi kuin musta.
- HUD-alueet muuttuvat, kun pelaaja vaihtaa asetuksia tai hahmon varusteita (esim. ammu on pidempi tekstiä).
  Alueet pitää siksi tehdä säädettäviksi.
- Pelin oma HUD voi piirtyä myös valikoiden päälle; tämä ei saa vaikuttaa paneeleihin.

## 5b. Palaute 17.11. (kypärätesti) ja toimenpiteet

| Havainto | Syy | Toimenpide |
|---|---|---|
| Keskustelupaneeli liian alhaalla | `dialogue_panel_y_m=-0.05` ja paneeli on alareunassa | Nostettu `0.15` m (ini). Säädettävissä |
| Teksti luettava, mutta sijainti pitää vakiinnuttaa | Paneeli on head-locked, ei pelinäkymän kiinteä kohta | Pidetään head-locked; sijainti asetuksilla `dialogue_panel_y_m` ja `_dist_m` |
| Kartta-markkerit ja muu HUD puuttuvat | `fHUDOpacity=0` piilottaa koko HUD:n | **Vaihe 1 (paneelit) ja Vaihe 2 (oma HUD) tekemättä.** Ei nopeaa korjausta |
| Pään kääntäminen tuplaa tai on epäsulava | Silmät vuorottelevat, jolloin kumpikin silmä päivittyy 30 Hz:llä, ja pose-korjaus ei kata pelikameran liikettä | Ei korjattu. Juurisyy on AFR-rakenne, ei yksittäinen asetus |
| Hipfire-ristikko liian pieni | Ristikko oli 1,2° | Kasvatettu 3,0°:een (`reticle_deg`) |
| Ristikko ei osu minne ammutaan | Ristikko on head-locked ja pelin tähtäys lähtee aseesta, ei päästä | Lisätty `reticle_yaw_deg` ja `reticle_pitch_deg` säätöön. Mittaus tarvitaan |

**Tehty 17.11.:** paneelin korkeus, ristikon koko ja ristikon siirtoasetukset. Proxy asennettu (`7120f20f…`).
**Ei tehty:** HUD-elementit (kartta, HP, ammu) ja pään kääntämisen sulavuus.

## 5c. Inspiraatio HUDFrameworkista (ei asennusta)

Päätös 17.11.: modia ei asenneta, mutta sen ideoita käytetään omassa toteutuksessa. Lähde: Nexus 20309
ja paketti `Downloads\HUDFramework 1.0f-20309-1-0f` (vain luettu). Ei kopioitu koodia repoon.

| Idea | Miten sovelletaan meillä |
|---|---|
| Jokainen HUD-elementti on oma widget, jolla on oma sijainti ja läpinäkyvyys | Proxy määrittelee jokaiselle elementille oman paneelin (`hud_hp_*`, `hud_ammo_*`, `hud_compass_*`) ja oman head-locked-sijainnin |
| Sijainti ja koko säädettävissä ilman koodimuutosta | Kaikki paneelit ini-asetuksilla, samaan tapaan kuin `dialogue_panel_*` |
| Elementtien piilotus yksitellen | Käytämme jo `fHUDOpacity=0`:aa; myöhemmin voi piilottaa vain osan, jos arvot luetaan itse |
| Arvot viestitään widgetille (`SendMessage`) | Vaihtoehto A: arvot on löydettävä pelistä. Vaihtoehto: plugin lukee arvot Papyrus-tyyppisesti RTTI:n kautta ja kirjoittaa ne jaettuun muistiin (`stereo_shm.h`) |
| Vaihtoehtoisten UI-tilojen tuki (esim. kompassi pois) | Jokaiselle elementille on oma `_enabled`-asetus |

**Mitä ei käytetä:** HUDFrameworkin Papyrus-skriptejä tai muokattua `HUDMenu.swf`:ää.

## 6. Mitä tarvitsen käyttäjältä

- Testi `fHUDOpacity=0`: häviääkö HUD? (Vaihe 0, kohta 1.)
- Hipfire-kuva, jossa HP ja ammu ovat näkyvissä, ilman keskustelua. (Vaihe 0, kohta 2.)
- Mielipide siitä, mitkä HUD-elementit ovat tärkeitä kypärässä (HP, ammu, kompassi, teksti).

## 7. Tila 17.11.2026

| Vaihe | Tila |
|---|---|
| 0. Mittaus: `fHUDOpacity=0` piilottaa HUD:n; alueet mitattu | valmis |
| 1. Paneelit proxyssa (HP, ammu, kompassi), peitto maailmasta | toteutettu ja asennettu, testattu osittain (näkyvät; vilkkuminen korjattu, tausta läpinäkyväksi) |
| 1b. Säätötila pelissä (Numpad) | toteutettu ja asennettu, käytössä |
| 2. Arvojen lukeminen (vaihtoehto A) | ei aloitettu |
| 3. SWF-patchaus: elementtien siirto | työkalut `tools/hud/`, testi-HUD asennettu `Data\Interface` (palautus: poista tiedosto) |
| 4. MCM: näkyvyys ja sijainti per elementti | suunniteltu, ei aloitettu (`docs/TODO.md`) |

Avoimet kohdat: testi-HUD:n toimivuus pelissä, kartta- ja tehtävämarkkerit, kolmannen persoonan tähtäyksen tuplakuva.
