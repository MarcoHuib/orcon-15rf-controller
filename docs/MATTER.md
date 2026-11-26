# Matter op de Heltec V2 (experimenteel)

Matter maakt de Orcon-controller bedienbaar vanuit Apple Woning, Google Home en andere slimme-woning-apps. Het is dezelfde firmware als de gewone controller (`src/`), gebouwd op **ESP-IDF 5.5** met **arduino-esp32 3.3.12** als component en **ESP-Matter v1.6**. Matter loopt alleen via het thuisnetwerk (on-network commissioning); Bluetooth staat uit.

Alles van de gewone controller blijft: de webinterface en het dashboard, WiFi, RF en de profielen, Siri, de eigenaarslogin, ondertekende OTA met proefstart en terugval, en de OLED.

> **Status: experimenteel.** De officiële v1.x-releases komen nog uit PlatformIO (`platformio.ini`). De Matter-firmware is **universeel**: het image bevat geen identiteit. Elke controller maakt zijn eigen Matter-identiteit zelf, bij het activeren, en bewaart die op het apparaat. Zo kan één image voor elk apparaat dienen en gepubliceerd worden. Een officiële Matter-release is er nog niet: de releasepijplijn volgt.

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
| Eén ventilator "Orcon ventilator" als gebridged apparaat, Afwezig = ventilator uit, Auto | Experimenteel (host- en QEMU-tests) |
| Standaardnaam bij koppelen, eerlijke fabrikant- en productgegevens | Experimenteel; Apple's gedrag is alleen uit bronnen bekend |
| `/connect`-pagina met QR-code, aftellende timer, controllerbeheer; Matter-kaart op het dashboard | Experimenteel (host-tests, QR-decoder, preview) |
| Updatecontrole via één herstart zonder Matter | Experimenteel (host-tests) |
| Stable- en bètakanaal | Experimenteel (host-tests; nog geen bètarelease) |
| Universele firmware: de identiteit op het apparaat maken, bewaren, controleren, en wissen bij een fabrieksreset | Experimenteel (QEMU: drie apparaten met hetzelfde image, negen starts; host-tests) |
| Overstap van een image met identiteit naar de universele firmware: identiteit, codes en namen blijven | Experimenteel (QEMU) |
| Eigen variant `control-matter`: een update voegt Matter nooit vanzelf toe en haalt het nooit vanzelf weg | Experimenteel (host-tests) |
| Releasenummers voor Matter-builds | Experimenteel (build); de releasepijplijn volgt |

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
- **Fabrieksreset.** Orcon's fabrieksreset wist de identiteit en alle koppelingen. De volgende eigenaar krijgt een nieuw apparaat met een nieuwe koppelcode.
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

Na stap 3 staat het overstap-image, met sleutel, nog in de andere slot als terugval. Installeer later nog één keer de universele firmware (of de volgende update): dan bevatten beide slots firmware zonder sleutel, en blijft terugvallen mogelijk.

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

Onder **Gekoppelde controllers** staan de gekoppelde apps. Verwijderen vraagt eerst om bevestiging en volgt hetzelfde pad als Matter's eigen `RemoveFabric`.

## Bediening in Apple Woning

| Orcon | In Woning |
|---|---|
| Stand 1, 2, 3 | de snelheidsregelaar (1–33 %, 34–66 %, 67–100 %; MultiSpeed met drie snelheden) |
| Auto | de Auto-schakelaar in het detailscherm van de ventilator |
| Afwezig | de ventilator **uit**; weer aan gaat terug naar de laatst gemelde stand, of Auto |

- Elk verzoek gaat door precies dezelfde toelating als de webknoppen en Siri (`ctlAdmit`): unit, profiel, identiteit, RF-budget, sessies.
- Woning toont wat de unit zelf meldt (31D9), nooit het verzoek. Een geweigerd verzoek valt dus terug op de werkelijke stand.
- Voor de eerste melding van de unit toont Woning Auto.
- Siri en automatiseringen werken met wat Woning toont: aan/uit en het percentage.

### Naam in Apple Woning

- Bij een apparaat zonder DCL-vermelding neemt Woning alleen de naam van een **gebridged** apparaat over: de NodeLabel uit Bridged Device Basic Information.
- Daarom is de ventilator een gebridged apparaat met de naam **Orcon ventilator**.
- De bridge zelf heet in Woning-instellingen → Woningcentrales en bruggen nog "Matter-accessoire".
- De NodeLabel van het apparaat en de naam in de mDNS-advertentie (`DN`) zijn ook "Orcon ventilator". Woning gebruikt die niet aantoonbaar, andere apps soms wel.
- Een naam die jij in een app kiest, blijft bij firmware-updates staan: de firmware zet zijn standaardnaam alleen als er nog geen naam is opgeslagen.

### Gevolgen voor een bestaande koppeling

Woning leest de samenstelling van een apparaat niet opnieuw in na een firmware-update. De overstap van de eerste opbouw (ventilator plus een stopcontact voor Afwezig) naar de gebridgede ventilator vraagt daarom **één keer opnieuw koppelen**: verwijder het accessoire in Woning en koppel opnieuw via `/connect`. Daarna blijft de opbouw vast, en houden updates de koppeling en je namen.

## Updates naast Matter

- Een TLS-verbinding naar GitHub vraagt meer geheugen dan er naast Matter vrij is, met de afgesproken reserve van 20 KB. De automatische updatecontrole staat daarom uit in Matter-builds.
- "Updatecontrole met korte herstart" op `/connect` start de controller één keer zonder Matter. Dan wordt meteen naar een nieuwe officiële versie gezocht, en daarna start de controller weer normaal: 60 s na een controle zonder nieuwe versie, en uiterlijk na 10 minuten.
- De vlag daarvoor staat in RTC-geheugen en geldt voor precies één softwareherstart.
- Lokale uploads (`.orconfw`, ondertekende OTA) blijven gewoon werken.
- De universele firmware is een eigen variant: `control-matter` (env `heltec_v2_matter`, herkenbaar aan een marker in het image). De updatecontrole biedt een Matter-controller alleen Matter-releases aan, en een controller zonder Matter alleen releases zonder Matter. Een update voegt Matter dus nooit vanzelf toe en haalt het nooit vanzelf weg.
- Wisselen kan alleen bewust: een lokale upload van een andere variant vraagt bevestiging, met uitleg wat er verandert. De controller weigert de wissel zonder die bevestiging.
- `tools/build_idf.sh --release N [--pre P]` stempelt een Matter-build als release (zoals de PlatformIO-release). Het publiceren via de releaseworkflow volgt.
- Borden met v1.0.x kennen `control-matter` nog niet. Wie via de webpagina naar Matter wil, heeft dus eerst een control-release met deze regels nodig; daarna kan de wissel.

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

- De CHIP-stack wordt niet verkleind: tijdens het koppelen bleef daar 1.824 B over.
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
  - de `/connect`-pagina (CSRF, codes, bevestiging, escaping);
  - de controle dat `orcon_connect_page.h` bij de HTML hoort (`tools/gen_connect_page.py --check`).
- `node test/matter/test_qr_decode.js`: de QR-encoder van de pagina tegen een onafhankelijke decoder (Chrome's BarcodeDetector; op macOS Apple's Vision). Synthetische codes, nooit de eigen code.
- `python3 -B tools/test_tx_poc_safety.py`: de bestaande veiligheidscontroles, uitgebreid voor Matter:
  - Matter bereikt RF alleen via `setupweb::matterControl` → `ctlAdmit`, met dezelfde vijf standen als Siri en precies één aanroeper;
  - de updatecontrole van een Matter-build vertrouwt alleen de vastgezette roots (`VERIFY_REQUIRED`; de DER-roots worden opnieuw gegenereerd en moeten gelijk zijn aan de PEM-roots);
  - mutaties bewijzen dat elke versoepeling van die regels faalt.
- `tools/qemu_memtest.sh`: de Matter-component in Espressif QEMU, als benadering van het bord (Ethernet in plaats van WiFi, geen RF; toelating en unit gesimuleerd; niets wordt gekoppeld). Eén image op drie geëmuleerde apparaten, elke start met een scenario:
  - A: activeren (identiteit op het apparaat gemaakt; namen, bediening en de labels van "een gebruiker", 53 controles), herstart (dezelfde identiteit en labels, 20), één opgeslagen byte veranderd, de schade gezien (Matter uit, het image bevestigt zich), een nieuwe identiteit door de eigenaar, Orcon's fabrieksreset (geen identiteit meer);
  - B: hetzelfde image op een tweede apparaat: een eigen identiteit;
  - C: een onderbroken activering, daarna een volledige;
  - over de starts heen: de identiteit blijft op A, verschilt tussen alle apparaten, en is nieuw na de vervanging. Elke keten slaagt bij `chip-cert validate-att-cert` (CHIP's eigen hulpmiddel, los van mbedTLS op het apparaat);
  - het laagste punt van de vrije interne heap zodra Matter klaar is, moet boven een ondergrens blijven: de gemeten waarde min 2 KB. Daaronder is de component of CHIP gegroeid;
  - `--migration`: een image met een (wegwerp)identiteit zet die op het apparaat; daarna vervangt de universele firmware het image (zoals een OTA), en blijven identiteit, codes en labels gelijk. DAC, PAI en declaration zijn byte voor byte die van het origineel;
  - `--nobridge`: dezelfde ventilator zonder bridge, voor de RAM-vergelijking.
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
