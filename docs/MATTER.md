# Matter op de Heltec V2 (experimenteel)

Matter maakt de Orcon-controller bedienbaar vanuit Apple Woning, Google Home en andere slimme-woning-apps. Het is dezelfde firmware als de gewone controller (`src/`), gebouwd op **ESP-IDF 5.5** met **arduino-esp32 3.3.12** als component en **ESP-Matter v1.6**. Matter loopt alleen via het thuisnetwerk (on-network commissioning); Bluetooth staat uit.

Alles van de gewone controller blijft: de webinterface en het dashboard, WiFi, RF en de profielen, Siri, de eigenaarslogin, ondertekende OTA met proefstart en terugval, en de OLED.

> **Status: experimenteel.** De Matter-firmware is **universeel** en bevat geen identiteit. Elke controller maakt bij het activeren zijn eigen Matter-identiteit en bewaart die op het apparaat. De releaseworkflow bouwt deze firmware voor het bètakanaal.

## Wat bewezen werkt op hardware, en wat nog niet

Getest op één Heltec WiFi LoRa 32 V2 (ESP32-D0WDQ6, 8 MB, geen PSRAM) in november 2026.

| Onderdeel | Status |
|---|---|
| Orcon op ESP-IDF 5 + Arduino 3: webinterface, login, WiFi, RF-ontvangst, profielen | **Bewezen** |
| Installatie via ondertekende OTA (bordgebonden manifest), proefstart, automatische terugval | **Bewezen**: een eerste Matter-build zonder werkende mDNS bevestigde zich niet en viel na 300 s vanzelf terug |
| `orcon15rf.local` en Matter's DNS-SD naast elkaar (gedeelde UDP-poort 5353) | **Bewezen**, ook na een WiFi-onderbreking |
| Koppelen met Apple Woning met eigen Matter-identiteit (handmatige code; toen nog in het image) | **Bewezen** |
| Ventilatiestand vanuit Apple Woning, via Orcon's eigen toelating (`ctlAdmit`) naar RF | **Bewezen**: de unit reageert op de snelheidsregelaar |
| Stabiliteit: 10,8 uur zonder herstart, Matter de hele nacht `ready`, geen geheugenlek | **Bewezen** (één nachtmeting) |
| Eén ventilator "Orcon ventilator" als gebridged apparaat, Afwezig = ventilator uit, Auto | Op de Heltec gezien in Woning, inclusief slider en Auto. Aantal tegels en herhaald koppelen vragen de bèta-2-proef. |
| Standaardnaam bij koppelen, eerlijke fabrikant- en productgegevens | De tegel heette op de geteste Heltec **Orcon ventilator**. |
| `/connect`-pagina met QR-code, aftellende timer, controllerbeheer; Matter-kaart op het dashboard | Experimenteel (host-tests, QR-decoder, preview) |
| Updatecontrole via één herstart zonder Matter | Experimenteel (host-tests) |
| Stable- en bètakanaal | Experimenteel (host-tests; nog geen bètarelease) |
| Universele firmware: de identiteit op het apparaat maken, bewaren, controleren, en wissen bij een fabrieksreset | Experimenteel (QEMU: drie apparaten met hetzelfde image, negen starts; host-tests) |
| Overstap van een image met identiteit naar de universele firmware: identiteit, codes en namen blijven | Experimenteel (QEMU) |
| Koppelen door een echte commissioner (chip-tool): attestatie, bediening, de controller blijft na een update | Experimenteel (QEMU, beide identiteiten: overgezet en op het apparaat gemaakt) |
| Eigen variant `control-matter`: een update voegt Matter nooit vanzelf toe en haalt het nooit vanzelf weg | Experimenteel (host-tests) |
| Releasenummers voor Matter-builds | Gecontroleerd door de releaseworkflow |

## Bouwen

Nodig: Docker, PlatformIO (`pip install platformio`, voor de vastgezette bibliotheken) en git.

```
tools/fetch_idf_deps.sh                          # eenmalig: bibliotheken zoals de PlatformIO-release ze gebruikt
tools/build_idf.sh                               # de universele Orcon + Matter-firmware (geen identiteit in het image)
tools/build_idf.sh --release 1002000 [--pre 1]   # hetzelfde als release 1.2.0 (of bèta 1 daarvan)
tools/build_idf.sh --no-matter                   # Orcon op ESP-IDF 5 zonder Matter
tools/build_idf.sh --identity ~/.orcon-matter    # alleen voor de overstap van een bestaand apparaat (zie "Overstap")
```

- De build draait in de vastgezette ESP-Matter-container (`espressif/esp-matter@sha256:94ed6a21…`, ESP-IDF 5.5.5).
- Het image komt in `idf/build/matter/orcon_15rf.bin` (genegeerd door git).
- `FW_VERSION` is de git-commit van de bronnen, net als bij een release. Een build met lokale wijzigingen heet `…-dirty`.
- Configuratie: `idf/sdkconfig.defaults` plus `idf/config/memory.defaults`, `clusters.defaults`, `matter.defaults` en `diagnostics.defaults`. Bootloader en partitietabel zijn gelijk aan v1.0.1 (`idf/partitions.csv` = Arduino's `default_8MB.csv`).
- De bibliotheken (ArduinoJson 7.4.3, RadioLib 7.8.1, U8g2 2.36.18) komen uit de pins in `platformio.ini`; arduino-esp32 3.3.12 uit git met gecontroleerde commit.

## De Matter-identiteit op het apparaat

Elk Matter-apparaat bewijst bij het koppelen wie het is met een eigen certificaat (DAC) en een privésleutel, en heeft een eigen koppelcode. De testset van de SDK is openbaar (sleutel, wachtwoordcode 20202021): die gebruikt deze firmware nooit.

**Matter activeren.** Op de Matter-pagina kiest de ingelogde eigenaar **Matter activeren**. De controller maakt dan eenmalig, op het apparaat zelf:

- een eigen keten PAA → PAI → DAC (met ESP-IDF's mbedTLS) en een certification declaration (met CHIP). CHIP controleert alles zelf voordat er iets wordt opgeslagen: het attestatieprofiel van elk certificaat, de keten, de DAC-sleutel en de handtekening van de declaration;
- een willekeurige koppelcode en discriminator (nooit 20202021 of 3840) met salt en SPAKE2+-verifier;
- een willekeurig serienummer (`ORC-xxxxxx`).

Alleen de DAC-sleutel blijft bewaard: de PAA- en PAI-sleutels worden één keer gebruikt en nooit opgeslagen. Daarna herstart de controller, en start Matter met die identiteit. Geen terminal, Docker of compiler nodig.

- **Opslag.** De NVS-namespace `orcon-id` in Matter's eigen NVS (48 KB in de ongebruikte spiffs-ruimte). Orcon's eigen NVS blijft onaangeroerd.
- **Heel of afwezig.** Een SHA-256 over alle velden wordt als laatste geschreven. Een onderbroken activering telt als "niet geactiveerd": de volgende activering begint opnieuw.
- **Beschadigd.** Een gewijzigd byte telt als "beschadigd". Matter blijft dan uit en niets vervangt de identiteit vanzelf: dat zou voor elke app een nieuw apparaat zijn. De eigenaar kiest zelf **Nieuwe identiteit maken**, na een bevestiging, en koppelt dan opnieuw.
- **Behouden.** Bij firmware-updates, herstarts en Matter's eigen reset (die wist alleen koppelingen).
- **Fabrieksreset.** Orcon's fabrieksreset wist de identiteit en alle koppelingen. De volgende eigenaar krijgt een nieuw apparaat met een nieuwe koppelcode. Dat doet ook firmware zonder Matter uit deze branch (na een terugstap van Matter): de opslag blijft na zo'n wissel staan, met de DAC-sleutel, tot een fabrieksreset (`orcon_matter_wipe.h`, in elke build). Alleen opslag die aantoonbaar van Matter is wordt gewist. Oudere firmware (v1.0.x) kent deze opslag niet en laat haar staan.
- **Geen hardwarebescherming.** Deze klassieke ESP32 draait zonder flash-encryptie (de bootloader blijft die van v1.0.1). Wie de flash via USB uitleest, leest ook de sleutel, net als bij een image met een ingebouwde identiteit.
- **Codes.** Alleen aan de ingelogde eigenaar, alleen bij een open koppelvenster, en nooit in een log.

**Grenzen.** Dit project heeft geen eigen vendor-ID en is niet door de CSA gecertificeerd. De identiteit gebruikt daarom het testbereik (vendor 0xFFF1, product 0x8000):

- Apple Woning meldt bij het koppelen een *niet-gecertificeerd accessoire*. Kies 'Voeg toch toe'.
- Het apparaat staat niet in de Matter DCL, dus Woning neemt geen officiële productnaam over (zie "Naam in Apple Woning").
- Een gecertificeerd product vraagt CSA-lidmaatschap, een eigen vendor-ID, certificering, een DCL-vermelding en DAC's van een vertrouwde PAA. Dat valt buiten dit project.

De firmware doet zich nergens voor als product van Orcon of als gecertificeerd: fabrikant "Zelfbouw (niet van Orcon)", product "15RF-controller voor Orcon" (`idf/components/orcon_matter/orcon_chip_config.h`).

## Overstap van een image met identiteit

Een controller met een van de eerste Matter-builds (de identiteit in het image) houdt zijn identiteit, koppelingen en namen zo:

1. Bouw eenmalig een overstap-image met die identiteit: `tools/build_idf.sh --identity ~/.orcon-matter`. Dat image is privé, want het bevat de DAC-sleutel: installeer het alleen op dat apparaat en deel het nooit.
2. Installeer het; vanaf een van de eerste Matter-builds met `--legacy-control` (zie "Installeren"). Bij de eerste start zet het zijn identiteit op het apparaat, en controleert het dat de codes daarna gelijk zijn.
3. Installeer daarna de universele firmware. Die vindt de identiteit op het apparaat: dezelfde sleutel, koppelcode en serienummer, dezelfde koppelingen en de namen die je koos.

Na stap 3 staat het overstap-image, met sleutel, nog in de andere slot als terugval. Zolang dezelfde identiteit op het apparaat staat, is dat geen extra blootstelling: de sleutel staat ook in de opslag. De terugval blijft dus bewaard tot de volgende update hem vervangt. Wil je hem eerder kwijt, installeer dan nog één keer de universele firmware: dan bevatten beide slots firmware zonder sleutel, en blijft terugvallen mogelijk.

Een fabrieksreset vervangt een overstap-image in de andere slot door een kopie van de draaiende, bevestigde firmware. Zo'n image draagt een eigen merkteken, `ORCON-MATTER-EMBEDDED-IDENTITY v1`; `tools/build_idf.sh` meldt "identity marker yes". Na de reset zou dat image anders de identiteit van de vorige eigenaar opnieuw op het apparaat zetten.

Grens: draait het overstap-image zelf tijdens de fabrieksreset, dan blijft de identiteit in dat image. Installeer daarom eerst de universele firmware, en geef een apparaat pas daarna door.

`tools/matter_identity.sh` maakt zo'n identiteit op de Mac. Voor een nieuw apparaat is dat niet meer nodig; de QEMU-test van deze overstap gebruikt het voor een wegwerpidentiteit.

## Installeren (met toestemming van de eigenaar)

1. Bouw de universele firmware (`tools/build_idf.sh`) vanuit een worktree zonder lokale wijzigingen: het image draagt die commit.
2. Controleer eerst, zonder sleutel en zonder wachtwoord:
   ```
   tools/orcon.py --host orcon15rf.local ota-install --image idf/build/matter/orcon_15rf.bin \
       --mac-file <bestand met de MAC van het bord> --tx-ack "control firmware" --dry-run
   ```
   - Het image moet een intact ESP32-image zijn (de ingebouwde SHA-256 klopt), gebouwd door `tools/build_idf.sh` uit de schone HEAD van deze worktree (geen `-dirty`), met die commit erin.
   - Met `--host` leest de controle `GET /api/ota` van het bord (alleen lezen, zonder login): welke slot de installatie schrijft, en dat het draaiende image in de andere slot de terugval blijft.
   - De MAC komt uit een lokaal bestand: hetzelfde `.flashtools/board.mac` dat de USB-installatie als verwachte MAC gebruikt, of het bestand van `--mac-file`. Nooit van het bord zelf.
3. Installeer met dezelfde opdracht, met `--yes` in plaats van `--dry-run`. Verder gaat het zoals elke installatie via de ondertekende OTA (`docs/OTA.md`):
   - het wachtwoord van de eigenaar;
   - een bordgebonden manifest (MAC en eenmalige nonce), ondertekend met de eigenaarssleutel, voor variant `control`;
   - het bord controleert handtekening, MAC, nonce, hash, markers en het ESP-IDF-image zelf.
4. De proefstart bevestigt zich alleen als Matter draait (binnen 300 s). Anders zet de bootloader het vorige image terug.
5. Een andere variant dan het bord draait installeer je alleen bewust: `--variant-change` (de webpagina vraagt het zelf, met uitleg wat er verandert). De eerste Matter-builds hebben variant `control`, de universele firmware `control-matter`.
   - Een bord met firmware van vóór die variant (v1.0.x, de eerste Matter-builds) kent `control-matter` nog niet en weigert het.
   - Daar installeer je het Matter-image één keer met `--legacy-control`: het wordt dan als `control` ondertekend. Dat bord controleert de control-markers, en die heeft een Matter-image ook.
   - Daarna gelden de nieuwe regels.
6. Terug naar v1.0.1 kan altijd met het officiële `.orconfw`: een ontwikkelbuild draagt release-stempel 0, dus elke officiële release is nieuwer. Dat is een wissel naar `control` (bevestigen); de Matter-gegevens blijven op het apparaat bewaard.

## Koppelen

1. Log in op de Orcon-pagina en kies op het dashboard de kaart **Matter** (pagina `/connect`).
2. Is Matter nog niet geactiveerd, kies dan eerst **Matter activeren**. De controller maakt zijn identiteit en herstart (ongeveer een halve minuut); log daarna opnieuw in.
3. Kies **Koppelvenster openen**. Het venster blijft 15 minuten open en sluit vanzelf na het koppelen.
4. Scan in Apple Woning (Voeg accessoire toe) de QR-code, of kies 'Meer opties' en voer de koppelcode in.

De QR-code wordt in de browser gemaakt (geen externe site of bibliotheek), uit de gegevens van het actieve venster. Koppelcodes staan alleen in het antwoord aan de ingelogde eigenaar, en alleen zolang een venster met de eigen code open is. Elk antwoord heeft `Cache-Control: no-store`, en de codes komen in geen enkele log.

Onder **Matter-koppelingen** staat één regel per fabric, met de naam bij bekende Vendor ID’s. Een fabric is een beveiligingsdomein. Voor één slimme woning kunnen meerdere fabrics bestaan. **Toegang intrekken** verwijdert selectief de lokale fabric, met een controle dat de tabel sinds de bevestiging gelijk is gebleven. Verwijder een achtergebleven Woning-bridge via Woning zelf. Zie de acceptatieproef verderop.

## Bediening in Apple Woning

| Orcon | In Woning |
|---|---|
| Stand 1, 2, 3 | de snelheidsregelaar (1–33 %, 34–66 %, 67–100 %; MultiSpeed met drie snelheden) |
| Auto | de Auto-schakelaar in het detailscherm van de ventilator |
| Afwezig | de ventilator **uit**; weer aan gaat terug naar de laatst gemelde stand, of Auto |

- Elk verzoek gaat door precies dezelfde toelating als de webknoppen en Siri (`ctlAdmit`): unit, profiel, identiteit, RF-budget, sessies.
- Een schrijfopdracht wijzigt eerst de Matter-instelling. Na afronding van de RF-transactie of weigering spiegelt de firmware de laatst bekende 31D9-stand. Een handmatige stand verschijnt dan als 33%, 66% of 100%. Uit betekent Afwezig. De Orcon blijft ventileren volgens zijn eigen Afwezig-stand.
- Bij Auto staan de Matter-instellingen op null. De werkelijke snelheid in Auto vraagt een gekalibreerde vertaling van de afzonderlijke toevoer- en afvoerwaarden uit 31DA. Bij een nieuwe installatie zonder unitmelding kan Auto als voorlopige stand verschijnen.
- Scènes kunnen als snelkoppeling dienen als Woning de gewenste stand opslaat. Test Siri, scènes en automatiseringen op de eigen Woning-versie, vooral voor Auto.

### Naam in Apple Woning

- Bij een apparaat zonder DCL-vermelding neemt Woning alleen de naam van een **gebridged** apparaat over: de NodeLabel uit Bridged Device Basic Information.
- Daarom is de ventilator een gebridged apparaat met de naam **Orcon ventilator**.
- De bridge zelf heet in Woning-instellingen → Woningcentrales en bruggen nog "Matter-accessoire".
- De NodeLabel van het apparaat en de naam in de mDNS-advertentie (`DN`) zijn ook "Orcon ventilator". Woning gebruikt die niet aantoonbaar, andere apps soms wel.
- Een naam die jij in een app kiest, blijft bij firmware-updates staan: de firmware zet zijn standaardnaam alleen als er nog geen naam is opgeslagen.

### Gevolgen voor een bestaande koppeling

Een oudere ontwikkelbuild met een zelfstandig fan-endpoint en een Afwezig-stopcontact kan een andere bridge- of tegelhistorie hebben. Alleen als zo’n oude indeling zichtbaar blijft en de bestaande koppeling stuk is, herstel je selectief via Woning en `/connect`. Een update vanaf de huidige gebridgede bèta gebruikt dezelfde layout. Laat daarbij een werkende koppeling staan.

## Updates naast Matter

- Een TLS-verbinding naar GitHub vraagt meer geheugen dan er naast Matter vrij is, met de afgesproken reserve van 20 KB. De automatische updatecontrole staat daarom uit in Matter-builds.
- "Updatecontrole met korte herstart" op `/connect` start de controller één keer zonder Matter. Dan wordt meteen naar een nieuwe officiële versie gezocht, en daarna start de controller weer normaal: 60 s na een controle zonder nieuwe versie, en uiterlijk na 10 minuten.
- De vlag daarvoor staat in RTC-geheugen en geldt voor precies één softwareherstart.
- Lokale uploads (`.orconfw`, ondertekende OTA) blijven gewoon werken.
- De universele firmware is een eigen variant: `control-matter` (env `heltec_v2_matter`, herkenbaar aan een marker in het image). De updatecontrole biedt een Matter-controller alleen Matter-releases aan, en een controller zonder Matter alleen releases zonder Matter. Een update voegt Matter dus nooit vanzelf toe en haalt het nooit vanzelf weg.
- Wisselen kan alleen bewust: een lokale upload van een andere variant vraagt bevestiging, met uitleg wat er verandert. De controller weigert de wissel zonder die bevestiging.
- Een Matter-controller leest zijn eigen update-index, `orcon-update-matter.json`. Een controller zonder Matter leest `orcon-update.json`, zoals v1.0.x dat al deed. Eén release kan zo beide varianten dragen, elk met een eigen ondertekend manifest.
- `tools/build_idf.sh --release N [--pre P]` stempelt een Matter-build als release (zoals de PlatformIO-release). Een bèta draagt zijn nummer in de stempel (`ORCON-RELEASE N-beta.P;`), en kan dus nooit voor de stabiele versie doorgaan.
- De releaseworkflow (`release-control.yml`) gebruikt `tools/release_ci.py` om beide varianten in één bètarelease te controleren, ondertekenen en verpakken. Stabiele releases bevatten voorlopig alleen `control`.
- Werk vanaf v1.0.0 of v1.0.1 eerst bij naar de stabiele control-release v1.0.2. Zet daarna *Bèta-updates ontvangen* aan, upload de `control-matter`-bundel en bevestig de wissel. Zie [Bètakanaal en Matter](RELEASES.md#bètakanaal-en-matter).

Bètaversies (`1.2.3-beta.N`) komen alleen binnen met de instelling "Bèta-updates ontvangen" (standaard uit). Een stable release staat altijd boven elke bèta van dezelfde versie.

## Geheugen

Statisch DRAM 118.576 B. Vrije heap op het bord in rust ongeveer 139,7 KB totaal en ongeveer 63–66 KB in de byte-adresseerbare interne heap; het laagste punt in een nacht was 119.136 B totaal.

| Maatregel | Waar |
|---|---|
| FreeRTOS-, heap- en ringbuffercode, I2C-ISR naar flash; assertions zonder bestandsnamen | `idf/config/memory.defaults` |
| Alleen de nodige Matter-clusters; kleinere Matter-eventbuffers | `idf/config/clusters.defaults`, `memory.defaults` |
| Matter-opslag in een eigen NVS in de ongebruikte spiffs-ruimte | `idf/config/matter.defaults` |
| TLS: dynamische buffers, de vastgezette roots via een callback in plaats van vooraf geparsed (−17,3 KB op de piek) | `matter.defaults`, `src/fw_update_roots_cb.h` |
| `/api/state` in de gedeelde antwoordbuffer; de 4 KB-buffer van de fabrieksreset op de heap | `src/net.cpp`, `src/owner_store.cpp` (alle builds) |
| De updategrens meet de byte-adresseerbare interne heap (niet de IRAM-only heap) | `src/fw_update_esp.cpp` (alle builds) |
| Geen stopcontact-endpoint meer: ongeveer 2 KB heap minder; de bridge kost ongeveer 1,4 KB terug (QEMU) | `orcon_matter.cpp` |

- De CHIP-stack wordt niet verkleind: tijdens het koppelen bleef daar 1.824 B over. In QEMU, met chip-tool als commissioner, blijft daar 1.716–1.952 B over, en zakt de vrije interne heap tijdens het koppelen van ongeveer 171,7 naar 161,9 KB.
- De pagina `/connect` (10,4 KB gzip) komt uit flash en kost geen RAM.
- De identiteit maken (eenmalig, vóór Matter start) gebruikt tijdelijk ongeveer 10 KB heap en duurt in QEMU ruim 6 s. Daarvoor is de firmware ongeveer 17 KB groter (X.509, CMS, SPAKE2+, de setup codes). Daarna kost de identiteit geen RAM: CHIP leest elk deel uit NVS wanneer het dat nodig heeft.
- De QEMU-geheugentest (`tools/qemu_memtest.sh`, ook in CI) bewaakt de Matter-component: het laagste punt van de vrije interne heap zodra Matter klaar is, is in QEMU ongeveer 173,2 KB met de bridge en 174,6 KB zonder. De test faalt onder 171.000 B. QEMU is een benadering: zonder WiFi en zonder koppeling ligt de heap daar hoger dan op het bord.

### De PlatformIO-builds ten opzichte van `main`

De officiële releases (zonder Matter), met dezelfde instellingen gebouwd uit `main` en uit deze branch:

| Build | RAM (statisch) | Flash |
|---|---|---|
| `heltec_v2_sx1276` (RX-only) | 85.356 → 75.116 B (−10.240) | 1.065.173 → 1.067.589 B (+2.416) |
| `heltec_v2_sx1276_control` | 88.564 → 78.348 B (−10.216) | 1.225.213 → 1.230.077 B (+4.864) |
| `…_webctl`, `…_webctl_dryrun`, `…_rftest` | −10.240 B | +2.420 tot +2.440 B |
| `…_tx_poc`, `…_tx_poc_dryrun` (geen webinterface) | gelijk | +128 / +132 B |

- RAM: de gedeelde antwoordbuffer (twee statische buffers van 3 KB minder) en de 4 KB-buffer van de fabrieksreset, die nu alleen tijdens een reset op de heap staat.
- Flash: de updateherstart en de stable/bètakanalen (`fw_update_esp`, `fw_update`, `ota_policy`: samen ongeveer 3,7 KB in de control-build), de Matter-variant en de bewuste wissel (`ota_policy`, de uploaddialoog), de stap van de fabrieksreset, en de Matter-kaart op het dashboard (zonder Matter verborgen).

## Beveiliging

- Koppelen alleen op verzoek van de ingelogde eigenaar: geen autostart, geen testparameters, geen testtriggers.
- Elke schrijfroute vraagt de sessie, het CSRF-token, JSON en de eigen herkomst (`setupweb::authorize(true)`).
- Codes alleen aan de eigenaar en alleen bij een open venster met de eigen code; nooit in publieke API's, logs of caches.
- Matter start alleen op het thuisnetwerk, nooit naast het setup-AP van Orcon.
- Een proefstart bevestigt zich alleen met draaiende Matter zodra er een geldige identiteit is. Zonder identiteit (nog niet geactiveerd) of met een beschadigde bevestigt een image zich zoals elke Orcon-firmware: geen ander image lost dat op.
- Matter activeren, en een nieuwe identiteit na schade: alleen de ingelogde eigenaar, met het CSRF-token, en nooit tijdens de proefstart van een update (de herstart zou die update terugdraaien).
- Ontwikkelbuilds dragen release-stempel 0; anti-rollback staat uit: de weg terug naar een officiële release blijft open.

## Tests

- `test/run_host_tests.sh`: alle bestaande suites plus `test/matter/`:
  - de bedieningsregels, de toestandsmachine, de NVS-eigendomscontrole, de updateherstart en de kanalen;
  - de begrensde tekst (`orcon_matter_out.h`) waarin elk Matter-antwoord en elke `[mem]`-regel wordt opgebouwd: elke `vsnprintf`-uitkomst wordt gecontroleerd en nooit wordt er voorbij de buffer geschreven; een te lang antwoord wordt een fout in plaats van kapotte JSON (grensgevallen: precies passend, één te veel, negatief, lege buffer, halve escapes);
  - de `/connect`-pagina (CSRF, codes, bevestiging, escaping);
  - de controle dat `orcon_connect_page.h` bij de HTML hoort (`tools/gen_connect_page.py --check`).
- `node test/matter/test_qr_decode.js`: de QR-encoder van de pagina tegen een onafhankelijke decoder (Chrome's BarcodeDetector; op macOS Apple's Vision). Synthetische codes, nooit de eigen code.
- `python3 -B tools/test_tx_poc_safety.py`: de bestaande veiligheidscontroles, uitgebreid voor Matter:
  - Matter bereikt RF alleen via `setupweb::matterControl` → `ctlAdmit`, met dezelfde vijf standen als Siri en precies één aanroeper;
  - de updatecontrole van een Matter-build vertrouwt alleen de vastgezette roots (`VERIFY_REQUIRED`; de DER-roots worden opnieuw gegenereerd en moeten gelijk zijn aan de PEM-roots);
  - mutaties bewijzen dat elke versoepeling van die regels faalt.
- `tools/qemu_memtest.sh`: de Matter-component in Espressif QEMU, als benadering van het bord (Ethernet in plaats van WiFi, geen RF; toelating en unit gesimuleerd). Eén image op drie geëmuleerde apparaten, elke start met een scenario:
  - A: activeren (identiteit op het apparaat gemaakt; namen, bediening en de labels van "een gebruiker", 53 controles), herstart (dezelfde identiteit en labels, 20), één opgeslagen byte veranderd, de schade gezien (Matter uit, het image bevestigt zich), een nieuwe identiteit door de eigenaar, Orcon's fabrieksreset (geen identiteit meer);
  - B: hetzelfde image op een tweede apparaat: een eigen identiteit;
  - C: een onderbroken activering, daarna een volledige;
  - over de starts heen: de identiteit blijft op A, verschilt tussen alle apparaten, en is nieuw na de vervanging. Elke keten slaagt bij `chip-cert validate-att-cert` (CHIP's eigen hulpmiddel, los van mbedTLS op het apparaat);
  - het laagste punt van de vrije interne heap zodra Matter klaar is, moet boven een ondergrens blijven: de gemeten waarde min 2 KB. Daaronder is de component of CHIP gegroeid;
  - `--migration`: een image met een (wegwerp)identiteit zet die op het apparaat; daarna vervangt de universele firmware het image (zoals een OTA), en blijven identiteit, codes en labels gelijk. DAC, PAI en declaration zijn byte voor byte die van het origineel;
  - `--nobridge`: dezelfde ventilator zonder bridge, voor de RAM-vergelijking;
  - `--commission`: een echte commissioner. chip-tool draait naast QEMU op een TAP-verbinding in dezelfde container, en vindt het apparaat via DNS-SD.
    - Eerst strikt, met de PAA van het apparaat als enige vertrouwde root. CHIP's attestatiecontrole moet slagen voor de handtekening van de DAC-sleutel over de uitdaging van de commissioner, de keten PAA → PAI → DAC, vendor en product, en de nonce. Daarna stopt ze op precies één fout, 601: geen vertrouwde sleutel voor de certificeringsverklaring. Een zelfgemaakte verklaring heeft geen door de CSA ondertekende signer; daarom meldt Apple Woning een niet-gecertificeerd accessoire.
    - Daarna met de toestemming van de eigenaar, zoals 'Voeg toch toe' in Woning. De controle draait opnieuw, meldt alleen 601, en het koppelen voltooit.
    - De verklaring zelf wordt apart gecontroleerd: OpenSSL controleert de CMS-handtekening met haar signer, en `chip-cert print-cd` de inhoud (vendor, product, apparaattype, certificeringstype, signer).
    - Dan leest chip-tool Basic Information en schrijft het FanMode, via Orcon's (gesimuleerde) toelating.
    - P: een overstap-image zet zijn identiteit op het apparaat; chip-tool koppelt het. Daarna wordt het app-slot met de universele firmware herschreven, zoals een OTA dat doet. De controller blijft en leest en schrijft via CASE.
    - Q: een identiteit die op het apparaat is gemaakt, gekoppeld.
    - Koppelcode en PAA komen uit de geëmuleerde flash (ESP-IDF's `nvs_tool`): geen logregel bevat een code.
- `python3 -I tools/check_privacy.py`: geen gegevens die niet openbaar mogen worden:
  - geen apparaat-ID's, MAC- of LAN-adressen, persoonlijke paden of privésleutels;
  - geen Matter-identiteit (DAC-sleutel, verifier, salt, certificaten, koppelcode), geen koppelcodes of QR-payloads (alleen de voorbeelden uit de CHIP-specificatie);
  - geen identiteits-, firmware-, sleutel- of ruwe logbestanden.
- `python3 -I tools/check_privacy.py --history BASE..HEAD`: dezelfde regels voor elke commit (toegevoegde bestanden, toegevoegde regels en berichten). Een geheim dat een latere commit weer verwijdert, blijft immers openbaar.
- `python3 -B tools/test_orcon_cli.py`: het OTA-gereedschap, ook de controles van een ESP-IDF-image.
- `.github/workflows/matter.yml`, vier jobs die los van elkaar draaien:
  - veiligheid en privacy: elke stap draait, ook als een andere stap of job faalt; de privacycontrole leest ook elke commit van de pull request;
  - alle hosttests;
  - een Matter-build met de plaatshouder-identiteit;
  - de QEMU-geheugentest.

  Geen geheimen; er wordt niets gepubliceerd.

## Matter bèta 2 — technisch rapport en acceptatie

### A. Oorzaakanalyse

De Heltec antwoordde op de openbare statusroute met `ready`, `server_ready=true`, fout `0x0` en actieve uptime. De openbare firmwarestatus gaf `cc6fe82f38407534b354658a9258262c1a90c169`. Het lokaal gevalideerde release-image met die commit meldt `control-matter` en `v1.1.0-beta.1`. Controleer de releasestempel van de draaiende installatie onder **Systeem → Firmware**, omdat de openbare firmwarestatus alleen de broncommit teruggeeft. De gemelde fabric-indices 7 en 8 kwamen uit de eigenaarssessie tijdens de eerste proef. De Vendor ID van fabric 8 en de tabel vóór commissioning ontbreken.

| Bevinding | Bewijs en resterende vraag |
|---|---|
| Apple Woning plus Apple Keychain kunnen samen op een Matter-apparaat staan | Onderzoekers lazen na Apple-commissioning beide fabrics uit met chip-tool. CSA kent `0x1384` toe aan Apple en de gepinde fabrikantentabel noemt die VID Apple Keychain. Verifieer op de Heltec of de tweede regel werkelijk VID 4996 heeft en wanneer deze is toegevoegd. |
| Fabric 7 en 8 zijn twee tabelindices | Een Index is een lokaal oplopende verwijzing, kan worden hergebruikt en zegt niets over het aantal fysieke ventilatoren of hubs. De oude paginacopy suggereerde ten onrechte één app per fabric. |
| Twee zichtbare Apple-tegels | Een eerdere endpointlayout en een achtergebleven bridge zijn concrete hypotheses. Vergelijk bridge-instellingen en Descriptor per operationele node met de bijgehouden fabric-events. Alleen twee fabrics verklaart twee tegels onvoldoende. |
| ‘Geen reactie’ en ‘Bijwerken…’ | De lokale server was bereikbaar tijdens deze meting. Daarmee blijven Apple-hub, CASE/subscriptions, operationele DNS-SD, WiFi/IPv6-herstel, een bewuste updateherstart en geheugenpieken open. Timing en diagnose per storing zijn nodig. |
| Wijziging binnen dezelfde snelheidsstand | In de gepinde SDK past een sliderwrite binnen de stand de setting aan zonder andere FanMode. De oude spiegeling sloeg dan de normalisatie van de setting over. Ook lag de On/Off-rapportage buiten de lock waarin de mailbox werd gecontroleerd. Beide paden zijn aangepast. |

[Apple MatterSupport](https://developer.apple.com/apple-home/matter/) · [Apple Support voor registraties en verbonden voorzieningen](https://support.apple.com/nl-nl/102135) · [NDSS 2025, § IV-A, met fabric-uitlezing](https://www.ndss-symposium.org/wp-content/uploads/2025-266-paper.pdf) · [CSA-vermelding 0x1384](https://csa-iot.org/csa_product/apple-proprietary-platforms-6/) · [Gepinde fabrikantentabel](https://github.com/espressif/connectedhomeip/blob/75902a0c6f6b208d5db963a8082423465384c594/src/app/zap-templates/zcl/data-model/manufacturers.xml)

**Model.** De Heltec draait één Matter-node, met Root Node (ep0), Aggregator (ep1) en Bridged Node plus Fan (ep2). De laatste twee vormen de bridge en haar zichtbare ventilator. Endpoint-ID’s volgen uit vaste aanmaakvolgorde. De bridge-fan krijgt een vaste UniqueID uit de STA-MAC. De standaardnaam wordt lokaal gezet en opgeslagen namen krijgen voorrang. Gebruik voor verificatie de Descriptor-lijsten `DeviceTypeList`, `PartsList`, `ServerList` en `ClientList` op ep0/1/2. Op ep2 controleer je daarnaast Reachable, UniqueID, NodeLabel, VendorName, ProductName en SerialNumber. Vergelijk de waarden lokaal vóór en na OTA. Deel het serienummer en de UniqueID alleen als het onderzoek ze vereist.

Een fabric wordt gekenmerkt door de trust root en Fabric ID. Elke node heeft binnen die fabric een operationele Node ID en credentials. Een commissioner brengt de node via een PASE-sessie met de setupcode in een fabric. Een administrator beheert fabrictoegang en kan een nieuw koppelvenster openen. Controllers gebruiken CASE om opdrachten te sturen of een subscription voor statuswijzigingen op te zetten. Eén fabric kan meerdere controllers bevatten, bijvoorbeeld een hub en andere nodes. De Apple Woning-app, woningcentrale en het Matter-accessoireregister in iOS hebben verschillende rollen. Voor de precieze verdeling van Apple-controllers binnen de fabrics van deze installatie ontbreekt een meting.

**Lifecycle.** In deze SDK gebruikt native `RemoveFabric` dezelfde `FabricTable::Delete` als lokale revocatie. De delegates ruimen onder meer ACL, groepsdata, CASE- en subscriptionresumption op, werken de commissioning window manager bij en trekken de operationele advertentie in. Een lokaal HTTP-verzoek heeft geen Matter-exchange om open te houden voor het antwoord. Daarom verlopen alle sessies van de verwijderde fabric zodra het indexnummer uit de tabel is. Sommige fouten in delegate-opslag worden door de SDK alleen gelogd. Controleer na een herstart of de fabric wegblijft en of het andere ecosysteem nog werkt. Het verwijderen van een bridge, een verbonden voorziening of een item uit Apple Sleutelhanger zijn afzonderlijke acties in Apple. Hun exacte effect op beide Heltec-fabrics moet per actie uit de tabel en events blijken.

[Gepinde native RemoveFabric](https://github.com/espressif/connectedhomeip/blob/75902a0c6f6b208d5db963a8082423465384c594/src/app/clusters/operational-credentials-server/OperationalCredentialsCluster.cpp) · [Gepinde server-delegates](https://github.com/espressif/connectedhomeip/blob/75902a0c6f6b208d5db963a8082423465384c594/src/app/server/Server.h)

### B. Architectuurbesluit

De gebruikte container bevat ESP-IDF 5.5.5 (`b774170ff46c393eeb5e495ea37936038d3f4f4f`), esp-matter v1.6 (`9f2e35ac3b3420fe81ee6834e72ec3db7bdba6a6`) en connectedhomeip (`75902a0c6f6b208d5db963a8082423465384c594`). Het model is getoetst aan de Matter 1.6-definities in die SDK.

| Architectuur | Bediening en ecosystemen | Impact en besluit |
|---|---|---|
| A. Eén bridged Matter Fan, MultiSpeed en Auto | Apple kan de gebridgede ventilator als tegel tonen, met slider en handmatig/Auto volgens de hardwareproef. Matter exposeert stand 1–3 en Afwezig. Home Assistant heeft een Matter Fan-implementatie. Google Home documenteert Fan Control en optioneel On/Off. | Gekozen. De bestaande optionele On/Off blijft de Aan/Afwezig-route. Stabiele endpoint-ID’s, identiteit en OTA-opslag. Laagste wijzigingsrisico voor bestaande koppelingen en geen extra clusteropslag. |
| B. Eén Fan met een andere featurecombinatie | Zonder On/Off blijven snelheid en FanMode mogelijk. Apple’s tegelgedrag en Siri-aan/uit met deze variant zijn onbeproefd. Step modelleert hoger/lager en definieert geen Apple-bediening. | Voor eventuele proef op apart testapparaat. On/Off schrappen bespaart een cluster, maar verandert de attribuut- en commandoset. Step voegt code en controllerafhankelijkheid toe. |
| C. A met Woning-scènes of automatiseringen | Eén tegel, met afzonderlijke snelkoppelingen voor standen die Woning kan opslaan. Apple ondersteunt scèneactivering via Siri. Test 33%, 66%, 100%, Aan/Uit en Auto op de installatie. | Aanvullende gebruikersoptie zonder firmware-, RAM-, flash- of OTA-impact. Automatiseringen en scènes zitten in het ecosysteem en moeten bij een herstel eventueel opnieuw worden geconfigureerd. |
| D. Fan met ModeSelect | ModeSelect modelleert benoemde keuzes. Er is geen aangetoonde generieke ondersteuning in Apple Woning voor een Fan. Google noemt specifieke Mode-clusters, zonder belofte voor deze combinatie. Home Assistant/openHAB-ondersteuning voor generieke ModeSelect op deze Fan is onbeproefd. | Een extra cluster maakt discovery, configuratie en testen complexer en gebruikt flash/RAM. Mogelijke aanpassing van caches of koppelingen zonder aangetoond UX-voordeel. |
| E. Native HomeKit/HAP | HAP heeft fan-snelheid en handmatig/Auto. Eigen discrete knoppen zijn ook hier niet aangetoond. Een HAP-pad zou eigen pairing en interoperabiliteit hebben. | Een tweede protocolstack verhoogt complexiteit, RAM en flash. Een overstap vraagt nieuwe koppelingen en raakt Multi-Admin. Geen extra stack voor deze bèta. |

**Wat Apple kan tonen.** `FanModeSequence=OffLowMedHighAuto` en `SpeedMax=3` waren al actief toen Apple een doorlopende slider toonde. `SpeedMax` bepaalt de snelheidsstappen op de server, zonder een Matter-mechanisme om Apple drie grafische stops of drie knoppen te laten tekenen. Low/Medium/High zijn direct schrijfbare Matter FanMode-waarden. Directe snelheidsknoppen in Woning zijn niet aangetoond. Beide percent-attributen zijn verplicht in Fan Control. MultiSpeed voegt de drie numerieke Speed-attributen toe, en Auto biedt null voor de settings. Het device type Fan 1.6 vereist daarnaast Identify en Groups. On/Off is optioneel. Extra clusters op hetzelfde endpoint kunnen extra mogelijkheden aan een controller bekendmaken. Hoe Apple die rendert, inclusief Fan zonder On/Off, blijft een platformtest. Gebruik scènes voor discrete toegang zolang de standaardtegel de slider gebruikt.

[Matter 1.6-downloads](https://csa-iot.org/developer-resource/specifications-download-request/) · [SDK Fan-device](https://github.com/espressif/connectedhomeip/blob/75902a0c6f6b208d5db963a8082423465384c594/data_model/1.6/device_types/Fan.xml) · [SDK Fan Control](https://github.com/espressif/connectedhomeip/blob/75902a0c6f6b208d5db963a8082423465384c594/data_model/1.6/clusters/FanControl.xml) · [Google Home Fan](https://developers.home.google.com/apis/android/supported-device-types) · [Home Assistant Fan](https://github.com/home-assistant/core/blob/dev/homeassistant/components/matter/fan.py) · [openHAB Matter](https://www.openhab.org/addons/bindings/matter/) · [Apple Woning en scènes](https://support.apple.com/nl-nl/guide/iphone/iph22d98bbca/ios)

### C. Implementatie en diagnostiek

- `orcon_matter_pairing.cpp` en `orcon_matter_history.h` registreren tot 24 events per boot. Gewone SDK-events en fabric-delegates leggen venster, PASE, pending AddNOC, commit, verwijdering, fail-safe-expiry en DNS-SD-initialisatie vast. `pending` gaat aan commit vooraf. PASE-stop en fail-safe-expiry hebben verschillende betekenissen. De snapshot geeft de actuele fail-safe-status. De SDK levert via deze events geen algemeen foutresultaat voor iedere mislukte poging. Een lokaal deleteprobleem bevat wel de SDK-foutcode als die beschikbaar is.
- Lokale revocatie vereist een huidige fabric-revisie, een expliciete bevestiging en bestaande owner/CSRF/Origin-controles. Tijdens een venster, commissioning, actieve schrijfactie of geplande herstart wacht de eigenaar tot de toestand stabiel is. Alle fabrics, waaronder onbekende VIDs, blijven selectief zichtbaar. De Basic- en Enhanced-commissioningroutes blijven beschikbaar voor Multi-Admin en de SDK bepaalt de venstersluiting.
- `orcon_matter.cpp` spiegelt de canonieke waarden na de RF-transactie en houdt On/Off en Fan Control onder dezelfde CHIP-lock als de mailboxcontrole. De delegate onderdrukt RF-echo’s bij intern spiegelen. Een native FanMode On volgt de SDK-semantiek (High). Aan via het optionele On/Off herstelt de laatste bevestigde geschikte stand, met Auto als terugval.
- `orcon_connect_page.html` en de gzip-header tonen Matter-koppelingen, VID, label en toevoegingen sinds start. De export gebruikt een afzonderlijke owner-only GET met `Cache-Control: no-store`. Fabrics en events worden compact weergegeven als respectievelijk `[index,vid,sinds_start]` en `[ms,event,index,vid,fout]`. `dropped` meldt hoeveel oudere ringbuffer-events zijn overschreven. De gedeelde buffer blijft 3.072 B.
- Het exportbestand bevat een actuele SDK-snapshot van de Fan, een afzonderlijke 31D9-unitstand, de laatste Matter-aanvraag en RF-busy. `202` betekent ingepland. De Orcon-console toont daarna RF-transmissie en afhandeling. `unit_mode=255` duidt een onbekende stand aan. Voor null of ontbrekende Fan-attributen staat `-1`. Reachable is de bridged-device-indicator en zegt niets over de Apple-verbinding. `wifi_up` en `ipv6_link_local` zijn lokale metingen. `dnssd-ready` zegt dat de SDK DNS-SD heeft geïnitialiseerd, zonder garantie dat de Apple-hub advertenties ontvangt. De snapshot bevat verder resetreden, vrije interne heap, minimum en grootste vrije blok. De bestaande seriële `[mem]`-metingen blijven beschikbaar voor taakstacks. Er zijn geen nieuwe sondes voor CASE- of subscriptionstatus per fabric.
- WiFi-configuratie, OTA-manifest en ondertekening, mDNS-poortdeling, RF-profielen, timer, toelating en identiteit/NVS-layout gebruiken de bestaande paden. De diagnose-export bevat geen labels, operationele IDs, certificaatmateriaal, codes of gebruikersreferenties. De live lijst escape’t tekstlabels. De eventbuffer gebruikt enkele honderden bytes statisch RAM, zonder extra taak of polling op de Heltec.

### D. Testresultaten en hardwarematrix

De lokale ESP-IDF-build levert 1.757.648 B firmware op. Het lokaal gevalideerde bèta-1-image was 1.753.872 B (+3.776 B). De linker rapporteert 119.072 B statisch DRAM tegenover 118.576 B in de eerdere projectmeting. De hostsuite geeft nul fouten, inclusief 66 browsertests. De build is een lokaal validatie-image en is niet geïnstalleerd op de Heltec.

PASS geldt alleen voor een uitgevoerde test. FAIL is een waargenomen verschil, BLOCKED vraagt een ontbrekende omgeving of handeling, NOT TESTED betekent dat de proef nog moet worden uitgevoerd. QEMU draait over Ethernet met gesimuleerde RF en is een aparte test van de SDK en het datamodel.

| Hardwareproef op bèta 2 | Verwachting | Status |
|---|---|---|
| Versie en bereikbaarheid | Apparaat meldt exact bijgewerkt firmwarecommit, juiste variant en Ready | BLOCKED voor bèta 2. Openbare uitlezing van de huidige bèta was PASS |
| Eerste koppeling, drie herhalingen | Eén actieve ventilator per Apple-woning. Verklaar elke fabric via VID en commit-event, en controleer venstersluiting | NOT TESTED |
| 0%, 1%, 33%, 34%, 66%, 67%, 100% | Afwezig, 1, 1, 2, 2, 3, 3. Verifieer unitmelding en uiteindelijke SDK-setting 0/33/66/100 | NOT TESTED |
| Stand 1/2/3, Auto, Uit, Aan | Bevestiging uit de unit. Uit = Afwezig. Aan herstelt vorige geschikte stand of Auto | NOT TESTED |
| Snelle slider en gelijktijdige webbediening | Laatste toegelaten aanvraag wint binnen RF-toelating. Geen extra RF-transmissies door mirroring | NOT TESTED |
| Originele afstandsbediening en webbediening | Op een betrouwbare 31D9-unitmelding volgen Woning en webinterface. Timer blijft beschikbaar | NOT TESTED |
| Siri, scène en automatisering | Apple voert de opgeslagen, daadwerkelijk ondersteunde stand uit | NOT TESTED |
| Heltec- en WiFi-herstart | Dezelfde naam, fan-UniqueID, fabric en werkende bediening. Tel tegels voor en na | NOT TESTED |
| Normale, getekende OTA | Zelfde identiteit, labels, credentials, fabric en RF-profielen. Gerichte rollback-proef via bestaande testopzet | NOT TESTED |
| Bridge verwijderen en opnieuw koppelen | Verwachte verwijdering zichtbaar in fabric-events. Een werkende ventilator komt terug | NOT TESTED |
| Tweede ecosysteem en selectieve revocatie | Beide werken vóór revocatie. De overblijvende fabric bedient de unit erna en na reboot | NOT TESTED |
| Mislukte commissioning | Na fail-safe-expiry komt een pending toevoeging niet blijvend in de tabel | NOT TESTED |
| Mobiel Apple Woning en firmwarewebsite | Bevestiging leesbaar en bereikbaar bij mobiel formaat | NOT TESTED op echte iPhone |

**Duurproef.** Neem bij 0, 1, 8, 24 en 72 uur een diagnose-export op en noteer beschikbaarheid in Woning, aantal bridges/tegels, RF-tx en een bevestigde unitstand. Vergelijk uptime, resetreden, heapminimum, grootste blok en fabric-events. Neem op het 1-uursmoment het gedrag tijdens een WiFi-onderbreking op. Test daarna afzonderlijk een router-, hub-, Heltec- en updatecontroleherstart. Registreer bij ‘Geen reactie’ eerst de huidige diagnostics en hubstatus voordat je herstart. Seriële `[mem]`-metingen kunnen CHIP-, loop- en RF-stack en geheugenpieken aanvullen. Alle duurproeven op bèta 2 staan op NOT TESTED.

### E. GitHub, versie en OTA

De actuele basis van dit werk is `release/matter-beta-public` op `ec727505c70df57ceaf455e785f96a7ca3616fc9`. PR #28 is beoordeeld op expliciete bevestiging, controle na Delete en documentatie. Sterke onderdelen zijn verwerkt in de bredere wijziging. Die PR is een concept en heeft geen bewijs dat Apple-dubbels verdwijnen. PR #27 bevat de uitrolarchitectuur. De bestaande workflow geeft releases uitsluitend via `main` uit, kiest nummers uit alle gepubliceerde releases, concepten en tags, en vereist de bestaande publicatiegoedkeuring. De publieke geschiedenis vermeldt op 27 november 2026 alleen v1.0.0 en v1.0.1. De geteste bèta 1 bestaat als lokaal gevalideerd image. Een publieke v1.0.2 dient eerst als stabiele overstap. Voor de privébèta-testinstallatie past v1.1.0-beta.2 als opvolger. Zonder een eerdere beta.1 in de GitHub-historie kiest de workflow eerst beta.1. Gebruik de gewone nummering, zonder een tag aan te maken alleen om het nummer op te hogen.

Bij een kanaalcontrole ontvangen gebruikers zonder opt-in alleen stabiele aanbiedingen. `control-matter` heeft een eigen variant. De workflow bouwt universeel, vergelijkt firmware/ELF en bordbestanden, controleert de manifesten en ondertekent via de bestaande goedgekeurde releasejob. De proefstart bevestigt alleen met Matter Ready en de bestaande OTA-voorwaarden. Er is in deze wijziging geen ondertekeningsstap, sleuteloverdracht, publicatie, partitiewijziging of migratie van bestaande endpoint-ID’s nodig.

[PR #27](https://github.com/MarcoHuib/orcon-15rf-controller/pull/27) · [PR #28](https://github.com/MarcoHuib/orcon-15rf-controller/pull/28) · [Releases](https://github.com/MarcoHuib/orcon-15rf-controller/releases) · [Releasehandleiding](RELEASES.md)

### F. Proef voor de eigenaar

1. Log in, noteer **Systeem → Firmware** (versie, commit, variant) en open Matter. Exporteer **Technische status → Diagnostiek kopiëren** zodra de uitgebrachte build deze optie heeft. Deel de export via het afgesproken kanaal. Vermijd het kopiëren van het gewone `/api/connect`-antwoord tijdens een open venster. Dat antwoord kan pairinggegevens bevatten.
2. Installeer een aangeboden, getekende `control-matter`-bèta via het bètakanaal. Kies op de Matter-pagina **Updatecontrole met korte herstart**, wacht tot de normale firmware is teruggekeerd en gebruik **Systeem → Firmware**. De bewuste updatecontrole kan een korte onderbreking in Woning veroorzaken. Gebruik op v1.0.0/v1.0.1 eerst de uitgebrachte stabiele overgangsversie volgens de releasehandleiding. Laat bestaande werkende Apple-koppelingen staan tijdens OTA en test ze direct daarna.
3. Voor een bestaande storing maak je vóór herstel een export en noteer je hoeveel Orcon-bridges, ventilatoren en verbonden voorzieningen zichtbaar zijn. Noteer het verloop van de Woning-status. Verwijder alleen de bedoelde Orcon-bridge via **Woninginstellingen → Woningcentrales en bridges** of vanuit de instellingen van de tegel via **Bridge**. Controleer ook **Instellingen → Algemeen → Matter-accessoires** voor de verbonden voorzieningen. Maak direct en na circa 30 seconden een nieuwe export. Bekijk eerst welke fabrics daadwerkelijk zijn verdwenen.
4. Trek alleen ondubbelzinnig achtergebleven lokale toegang in. Apple Keychain kan naast Apple Woning bestaan. Laat werkende of onbekende fabrics staan totdat hun rol vaststaat. Controleer dat een ander ecosysteem vóór en na de actie reageert. Een lokale verwijdering kan een tegel in Apple Woning laten staan.
5. Voor de repro kies je een geschikt testmoment. Leg eerst de uitgangstabel vast. Als de Apple-bridge verdwenen is en de Apple-uitgangssituatie bekend is, open je één venster en voeg je Orcon eenmaal toe. Bij een extra ecosysteem vraag je bij voorkeur een nieuwe code via de al gekoppelde app. Exporteer direct na het koppelen en na één herstart. Beoordeel de volgorde `window-open` → `pase-start` → `fabric-pending` → `fabric-commit` / `complete` → `window-closed` met de werkelijk waargenomen timestamps. De SDK kan eventnotificaties asynchroon afleveren. Noteer per poging het aantal tegels en hun bereikbaarheid. Herhaal drie keer als de betreffende testkoppeling veilig kan worden verwijderd. Voor een niet-reagerende oude bridge volstaat selectief herstel. Fabrieksreset en nieuwe identiteit zijn afzonderlijke, ingrijpende acties.
6. Zonder login zijn de gezondheid en broncommit veilig op te vragen. Kies het adres dat lokaal werkt. De tweede opdracht filtert de JSON direct tot de toegestane velden.

   ```sh
   ORCON_BASE=http://orcon15rf.local   # of het lokale adres van je controller
   curl -fsS --max-time 5 "$ORCON_BASE/api/connect/state"
   curl -fsS --max-time 5 "$ORCON_BASE/api/setup/state" |
     python3 -c 'import json,sys; print(json.load(sys.stdin).get("fw", "onbekend"))'
   ```
