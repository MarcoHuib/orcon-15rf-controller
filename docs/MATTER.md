# Matter op de Heltec V2 (experimenteel)

Matter maakt de Orcon-controller bedienbaar vanuit Apple Woning, Google Home en andere slimme-woning-apps. Het is dezelfde firmware als de gewone controller (`src/`), gebouwd op **ESP-IDF 5.5** met **arduino-esp32 3.3.12** als component en **ESP-Matter v1.6**. Matter loopt alleen via het thuisnetwerk (on-network commissioning); Bluetooth staat uit.

Alles van de gewone controller blijft: de webinterface en het dashboard, WiFi, RF en de profielen, Siri, de eigenaarslogin, ondertekende OTA met proefstart en terugval, en de OLED.

> **Status: experimenteel.** De officiële v1.x-releases komen nog steeds uit PlatformIO (`platformio.ini`). Matter-builds dragen release-stempel 0 en worden niet als release gepubliceerd. Een Matter-image bevat de DAC-privésleutel van precies één apparaat: zo'n image publiceer je nooit.

## Wat bewezen werkt op hardware, en wat nog niet

Getest op één Heltec WiFi LoRa 32 V2 (ESP32-D0WDQ6, 8 MB, geen PSRAM) in november 2026.

| Onderdeel | Status |
|---|---|
| Orcon op ESP-IDF 5 + Arduino 3: webinterface, login, WiFi, RF-ontvangst, profielen | **Bewezen** |
| Installatie via ondertekende OTA (bordgebonden manifest), proefstart, automatische terugval | **Bewezen**: een eerste Matter-build zonder werkende mDNS bevestigde zich niet en viel na 300 s vanzelf terug |
| `orcon15rf.local` en Matter's DNS-SD naast elkaar (gedeelde UDP-poort 5353) | **Bewezen**, ook na een WiFi-onderbreking |
| Koppelen met Apple Woning met eigen Matter-identiteit (handmatige code) | **Bewezen** |
| Ventilatiestand vanuit Apple Woning, via Orcon's eigen toelating (`ctlAdmit`) naar RF | **Bewezen**: de unit reageert op de snelheidsregelaar |
| Stabiliteit: 10,8 uur zonder herstart, Matter de hele nacht `ready`, geen geheugenlek | **Bewezen** (één nachtmeting) |
| Eén ventilator "Orcon ventilator" als gebridged apparaat, Afwezig = ventilator uit, Auto | Experimenteel (host- en QEMU-tests) |
| Standaardnaam bij koppelen, eerlijke fabrikant- en productgegevens | Experimenteel; Apple's gedrag is alleen uit bronnen bekend |
| `/connect`-pagina met QR-code, aftellende timer, controllerbeheer; Matter-kaart op het dashboard | Experimenteel (host-tests, QR-decoder, preview) |
| Updatecontrole via één herstart zonder Matter | Experimenteel (host-tests) |
| Stable- en bètakanaal | Experimenteel (host-tests; nog geen bètarelease) |

## Bouwen

Nodig: Docker, PlatformIO (`pip install platformio`, voor de vastgezette bibliotheken) en git.

```
tools/fetch_idf_deps.sh                          # eenmalig: bibliotheken zoals de PlatformIO-release ze gebruikt
tools/build_idf.sh --identity ~/.orcon-matter    # Orcon + Matter met de eigen identiteit van dit apparaat
tools/build_idf.sh                               # zonder identiteit: alleen om te compileren (Matter start dan nooit)
tools/build_idf.sh --no-matter                   # Orcon op ESP-IDF 5 zonder Matter
```

- De build draait in de vastgezette ESP-Matter-container (`espressif/esp-matter@sha256:94ed6a21…`, ESP-IDF 5.5.5).
- Het image komt in `idf/build/matter/orcon_15rf.bin` (genegeerd door git).
- `FW_VERSION` is de git-commit van de bronnen, net als bij een release. Een build met lokale wijzigingen heet `…-dirty`.
- Configuratie: `idf/sdkconfig.defaults` plus `idf/config/memory.defaults`, `clusters.defaults`, `matter.defaults` en `diagnostics.defaults`. Bootloader en partitietabel zijn gelijk aan v1.0.1 (`idf/partitions.csv` = Arduino's `default_8MB.csv`).
- De bibliotheken (ArduinoJson 7.4.3, RadioLib 7.8.1, U8g2 2.36.18) komen uit de pins in `platformio.ini`; arduino-esp32 3.3.12 uit git met gecontroleerde commit.

## Eigen Matter-identiteit (veilig)

Elk Matter-apparaat bewijst bij het koppelen wie het is met een eigen certificaat (DAC) en een privésleutel, en heeft een eigen koppelcode. De testset van de SDK is openbaar (sleutel, wachtwoordcode 20202021): die gebruik je nooit op je thuisnetwerk. Maak daarom per apparaat één keer een eigen identiteit:

```
tools/matter_identity.sh                         # -> ~/.orcon-matter (0700)
```

- Het script draait offline in de vastgezette container. Het maakt een eigen PAA → PAI → DAC-keten, een ondertekende certification declaration, een willekeurige koppelcode en discriminator met de SPAKE2+-verifier, en een willekeurig serienummer.
- Alles blijft in `~/.orcon-matter`, **buiten** het repository: `orcon_matter_creds.h` (voor `build_idf.sh --identity`), `identity.defaults` (serienummer), `KOPPELCODE.txt` en de sleutels. Het repository negeert die bestanden ook (`.gitignore`), maar zet ze er nooit in.
- Een bestaande identiteit wordt nooit vervangen. Een nieuwe identiteit is voor elke gekoppelde app een nieuw apparaat: maak dus een back-up van `~/.orcon-matter` (versleuteld).
- Een image gebouwd met een identiteit bevat de DAC-privésleutel en de koppelcode van dat apparaat. Installeer het alleen op dat apparaat en deel het nooit.
- Zonder identiteit gebruikt de build `idf/components/orcon_matter/placeholder/`: het image compileert en draait Orcon, maar Matter weigert te starten (`no-identity`). Op een bord bevestigt zo'n image zichzelf daardoor nooit en valt het vanzelf terug.

**Grenzen.** Dit project heeft geen eigen vendor-ID en is niet door de CSA gecertificeerd. De identiteit gebruikt daarom het testbereik (vendor 0xFFF1, product 0x8000):

- Apple Woning meldt bij het koppelen een *niet-gecertificeerd accessoire*. Kies 'Voeg toch toe'.
- Het apparaat staat niet in de Matter DCL, dus Woning neemt geen officiële productnaam over (zie "Naam in Apple Woning").
- Een gecertificeerd product vraagt CSA-lidmaatschap, een eigen vendor-ID, certificering, een DCL-vermelding en DAC's van een vertrouwde PAA. Dat valt buiten dit project.

De firmware doet zich nergens voor als product van Orcon of als gecertificeerd: fabrikant "Zelfbouw (niet van Orcon)", product "15RF-controller voor Orcon" (`idf/components/orcon_matter/orcon_chip_config.h`).

## Installeren (met toestemming van de eigenaar)

1. Bouw met de eigen identiteit van het apparaat (zie hierboven).
2. Installeer via de bestaande ondertekende OTA (`docs/OTA.md`): een bordgebonden manifest, ondertekend met de eigenaarssleutel, voor variant `control`. Het bord controleert handtekening, MAC, nonce, hash, markers en het ESP-IDF-image zelf.
3. De proefstart bevestigt zich alleen als Matter draait (binnen 300 s). Anders zet de bootloader het vorige image terug.
4. Terug naar v1.0.1 kan altijd met het officiële `.orconfw`: een Matter-image draagt release-stempel 0, dus elke officiële release is nieuwer.

> **Open punt.** `tools/orcon.py ota-install` verwacht nu nog een PlatformIO-pakket. Een pad voor IDF-images (`--image`) volgt in deze PR; tot dan gebeurt de installatie met het lokale hulpscript van de ontwikkelomgeving.

## Koppelen

1. Log in op de Orcon-pagina en kies op het dashboard de kaart **Matter** (pagina `/connect`).
2. Kies **Koppelvenster openen**. Het venster blijft 15 minuten open en sluit vanzelf na het koppelen.
3. Scan in Apple Woning (Voeg accessoire toe) de QR-code, of kies 'Meer opties' en voer de koppelcode in.

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

## Beveiliging

- Koppelen alleen op verzoek van de ingelogde eigenaar: geen autostart, geen testparameters, geen testtriggers.
- Elke schrijfroute vraagt de sessie, het CSRF-token, JSON en de eigen herkomst (`setupweb::authorize(true)`).
- Codes alleen aan de eigenaar en alleen bij een open venster met de eigen code; nooit in publieke API's, logs of caches.
- Matter start alleen op het thuisnetwerk, nooit naast het setup-AP van Orcon.
- Een proefstart bevestigt zich alleen met draaiende Matter. Een image zonder eigen identiteit start Matter nooit.
- Release-stempel 0, anti-rollback uit: de weg terug naar een officiële release blijft open.

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
- `python3 -I tools/check_privacy.py`: geen apparaat-ID's, MAC- of LAN-adressen, persoonlijke paden of privésleutels in het repository.
- `.github/workflows/matter.yml`: dezelfde hosttests, de veiligheidscontroles, de privacycontrole en een Matter-build met de plaatshouder-identiteit (geen geheimen, publiceert niets).
