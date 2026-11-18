# VR-porttausreferenssi (kerätty 16.11.2026)

Konteksti: F4-Flat-2-VR tekee porttauksen **injektiolla** (d3d11.dll-proxy + F4SE-plugin), ei Unityssä/UE:ssa.
Meidän arkkitehtuuriaan vastaavat siis erityisesti alla olevat injectio-pohjaiset projektit; Unity/UE-dokut ovat
taustaa stereo-renderöinnin periaatteille (multi-pass = meidän malli).

## Suoraan meidän malliimme sopivat (injectio-pohjaiset)

| Projekti | Mitä katsottava | Liittyy meidän osioon |
|---|---|---|
| elliotttate/vrframework | 17-osainen guide: injection, AFR, projection, TAA-ongelmat, HUD, frame timing, submission | **Osat 07–11** = meidän proxy (submit/bounds/AFR). Erityisesti AFR-paritus ja per-eye projection |
| praydog/REFramework | RE Engine VR-modien perusta: per-eye tekstuuri + submit + HUD kerrokset | Proxy-rakenne, overlay/2D-ruutu |
| dariulone/cyberpunk-vr-port | Aito stereo + ADS/ase; dokumentoi tuplakuva-ongelmia UI:ssa ja tähtäyksessä | **ADS-tuplakuvan juurten eristys** (meidän vaihe B.5) |
| jplakon/WorldWarVR | Fyysiset scopet, two-hand handling, aim-korjaukset | Kiikarit (meidän vaihe C.8) |
| Firejumper93/GhostReconWildlandsVR | Magnified overlay -scope (PiP) kun 3D-scope liian vaikea | Vaihtoehto meille: scope → 2D/overlay kuten Pip-Boy nyt |

## vrframework guide 07 (Frame Timing) — olennaiset opit (16.11)

1. **AFR-pariteetti on koko illuusion perusta:** "engine frame counter's parity must stay locked to the eye the
   compositor is expecting." Pariteetin lipsahtaminen = silmät desynkronoituvat → tuplakuva/epämukavuus.
   Meidän `eye_delay`/`swap_eyes` = REFrameworkin mutatoitava `m_left_eye_interval` (pariteetti→silmä -kuvaus,
   joka on tarkoituksella muutettavissa resynkia varten).
2. **Kolme laskuria, ei yksi:** engine / render / presenter tickaavat omassa tahdissa (putkitus). Meillä:
   pluginin update-seq (engine), proxyn present-seq (presenter) — render-laskuria ei erikseen. Jos näiden
   rytmi ajautuu, silmäparitus lipsahtaa — täsmälleen se off-by-one, jonka olemme diagnosoineet.
3. **Drift-tunnistus + skip-present (Starfield-malli):** jos laskurit lipsahtavat, korjaus on pudota
   TÄSMÄLLENEN yksi present (`m_skip_next_present`) — pariteetti siirtyy yhdellä ja L/R palaa kohdalleen.
   Lämmitysportti >100 framea ennen arviointia (käynnistys-/lataustilanne saa lipsua vapaasti).
4. **"Verify with a counter readout, not your eyes":** loki(laskurien) tila per frame; laskurin hyppy kahdella
   tai pariteetin flip keskellä sessiota ON judder, ennen kuin ihminen sen huomaa. Tukee suunnitelmamme:
   per-frame-sisältövertailu lokiin (tex[0] vs tex[1]) ini-käännösten epäonnistuessa.
5. Pariteetin on oltava SAMA kolmessa paikassa: view-injektio, submit-paritus JA TAA-historiabufferien avain.
   Jos mikään kolmesta eroaa, ne tappelevat keskenään.

## vrframework guide 10 (Submission & TAA) — olennaiset opit (16.11)

1. **Submit on kopio:** engine-backbuffer ei koskaan mene suoraan compositorille. Meillä jo näin (g_eyeTex).
   Triple-bufferaus + fence per tekstuuri: compositori lukee asynkronisesti — ei stompaa.
2. **TAA/AFR-saastuminen on isoin artifact-luokka:** "previous frame" on AFR:ssä TOINEN silmä → TAA sekoittaa
   silmät → ghosting/doubling reunoilla, kasvaa IPD:n mukana. Korjaus EI ole TAA pois vaan per-silmä historia:
   (a) past-projektioviive kaksi framea (same-eye), (b) historiatesuurien ping-pong pariteetilla (SwapBuffer).
   - Meillä: BMP-kaappaukset osoittivat yksittäisen still-framen PUHTAAKSI → TAA-historiasaastuminen ei
     selitä monokulaarista flippiämme (se näkyisi itse framessa). Varattu silti hypoteesiksi liikkuvaan aseeseen.
   - ADS/mono-tilassa meidän molemmat framet ovat identtisiä head-centered → TAA-historia on effectively
     same-eye → ADS:n pitäisi olla TAA-suhteessa puhtain tila.
3. **Menu/flat-UI → quad; mirror → stabiloitu yksisilmäinen:** REFrameworkin `m_desktop_fix` kopioi edellisen
   backbufferin ei-submitoivalle framelle, jotta työpöytäpeili ei välky silmästä toiseen. Meidän mirror-tupla
   EI ole tämä (meillä mirror = yksi kuva joka sisältää tuplan).
4. **Resync-mekanismi kuuluu designiin:** pariteetti→silmä -kuvaus pidetään muuttuvana juuri siksi, että
   framet lipsahtavat ajoittain ja kuvaus pitää voida kääntää ilman rebuildia — meidän ini-käännökset
   (eye_delay, swap_eyes) ovat tämän suora vastine.
5. Checklist-järjestys meidän tilallemme: 1–4 (submit toimii, BMP:t puhdas) → seuraavaksi 5–7 (temporal-
   resurssien metsästys) VAIN jos silmäparitus on varmistettu oikeaksi — muuten vianetsintä menee harhaan.

## Stereo-renderöinnin periaatteet (Unity/UE-dokut, koskevat meitä periaattelinja)

1. **Multi-pass (meidän malli):** per-silmä render eri frameissa (AFR). Yhteensopiva, mutta puolet kuvataajuudesta
   per silmä — meillä jo tunnettu rajoite (PLAN kohta 3). Single-pass instanced ei ole saatavilla injektiolla
   ilman moottorimuutoksia → ei meidän tie.
2. **Custom-shaderit tuplakuvana:** Unity/UE-ongelma "shader ei kirjoita molempia silmiä" EI päde meihin suoraan,
   mutta VASTINE on meillä: stereo-patchin matemaattiset bufferit (VP*W, kamerapositio) — jos pelin shader/build
   käyttää omaa matriisia jota patch ei näe, ase tähtäystä varten renderöityy väärin. Meidän ADS-tapaus
   ("ase kahdesti / väärin") voi olla tämä.
3. **Scopes:** oikea 3D-scope (vergence + magnificaatio) on vaikea; hyväksytyt ratkaisut: magnified overlay/PiP,
   red-dot, tai 2D-näyttö. Meillä Pip-Boy-malli jo toimii → scopeille sama (zoom=2 → 2D-ruutu).
4. **HUD/UI:** ei koskaan tuplana — meillä HUD on pelin flat-frameen sisältyvä → 2D-ruutu/bounds hoitaa.

## Aseen tähtäys VR-porteissa (käytännöt)

- Barrel+sights oikein molemmille silmille; near-object fusion kriittinen (~1–2 m) → meillä convergence_m
  (nykyisin 0 = pois; stereo hipfire OK, ADS mono).
- Moderni porta: two-hand grip + decoupled aim — motion-ohjaimille (meillä myöhemmin, FRIK-ideat ei GPL-koodia).

## Yhteys tämän hetken vikaan (16.11)

Meidän monokulaarinen tuplakuva EI ole Unity-tyylinen shader-tupla: BMP:t todistavat yksittäisen framen.
Se on AFR-parituksen off-by-one (eye_delay) → ajallinen flip. Ks. PLAN.md kohta 4 (16.11 klo ~12.3x).

## Lähteet (alkuperäinen lista)

- Unity: https://docs.unity3d.com/Manual/xr-stereo-rendering.html ,
  https://docs.unity3d.com/Manual/SinglePassStereoRendering.html ,
  https://docs.unity.com/en-us/engine/6000.6/manual/xr/graphics/stereo-rendering/single-pass-instancing ,
  https://developers.meta.com/vr/documentation/unity/unity-single-pass/
- Unreal: https://docs.unrealengine.com/xr-best-practices-in-unreal-engine/
- GitHub: elliotttate/vrframework, praydog/REFramework, dariulone/cyberpunk-vr-port, jplakon/WorldWarVR,
  Firejumper93/GhostReconWildlandsVR, BastiaanOlij/godot-vr-weapons, dilmerv/VRWeaponController, Zenukain/AngularScope
