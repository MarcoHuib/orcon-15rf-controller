# Heltec V2: volledig lokale USB-release

Deze procedure is bedoeld voor de Heltec V2 met klassieke ESP32-D0WDQ6, SX1276
en 8 MB flash. Selecteer de afzonderlijke dry-runomgeving; het projectbrede
PlatformIO-default is een ander bord. De RF-omgeving blijft compile-time verboden.
Geen GitHub Actions, artifactdownloads, remote CI-status of PlatformIO-upload.

## Lokale voorbereiding, zonder bordtoegang

Werk vanuit de repositoryroot op de beoordeelde herstelcommit. De exacte SHA en
resultaten van de uitgevoerde review staan in de lokale `build/codex-final/`
bewijsmap. Gebruik voor deze release de SHA uit dat RESULTS.json, niet zomaar
een later gewijzigde HEAD. Controleer dat `git status --porcelain` leeg is.

De bestaande lokale esptool 4.8.1-installatie, het gepinde sdist-archief,
pyserial 3.5 en intelhex 2.3.0 moeten aanwezig zijn in `.flashtools/`.
De tool controleert de archivehash, de werkelijk uitgevoerde Pythonbronnen en
dependencyversies. Bij ontbreken wordt afgebroken; er wordt niets gedownload.
Voor builds moeten de lokale PlatformIO-platformen, toolchains en bibliotheken
beschikbaar zijn. De runner bouwt uitsluitend expliciete omgevingen, nooit upload.

```bash
SHA="$(git rev-parse HEAD)"
DRY=heltec_v2_sx1276_tx_poc_dryrun
RX=heltec_v2_sx1276
EVIDENCE=build/codex-final
# Gebruik een nieuwe bewijsmap bij herhaling; overschrijf bewaarde releasebewijzen niet.
pwsh -NoProfile -File tools/test_local.ps1 -OutputDirectory "$EVIDENCE" -Firmware
# Controleer: runner exit 0, RESULTS.json.commit == SHA, dirty leeg, alle checks PASS.
tools/heltec_v2.sh package "$EVIDENCE" "$DRY"
tools/heltec_v2.sh package "$EVIDENCE" "$RX"
LOCAL_ENV="$DRY" tools/heltec_v2.sh verify "$SHA"
LOCAL_ENV="$DRY" tools/heltec_v2.sh plan "$SHA"
LOCAL_ENV="$RX" tools/heltec_v2.sh plan "$SHA"
env -u LOCAL_ENV tools/heltec_v2.sh verify rollback
env -u LOCAL_ENV tools/heltec_v2.sh plan rollback
```

Op een sandbox waarbij de globale PlatformIO-cache niet schrijfbaar is, gebruik
een werkruimtekopie van `packages`, `platforms` en de bestaande `.cache`, met
`PLATFORMIO_CORE_DIR="$PWD/build/pio-core"` en
`PLATFORMIO_SETTING_ENABLE_TELEMETRY=no`. Wijzig geen systeemrechten of beveiliging.

Het lokale pakket is `.flashtools/local/<SHA>-<omgeving>/`. Controle omvat alle
vereiste tests (ook UBSan, web, vijf builds, flashsimulaties), exitcodes, schone
broncommit, bronstabiliteit tijdens de run en hashes van bewijsbestanden en vier
images. Manifestleden mogen niet ontbreken of dubbel voorkomen. Zowel bootloader
als applicatie moeten een geldige klassieke ESP32-imagechecksum en SHA-256 hebben,
met DIO/8 MB/40 MHz-headers. De partitietabel moet exact overeenkomen, inclusief MD5,
labels, types, offsets, groottes, flags en padding. Alleen de twee V2-omgevingen
zijn toegelaten; dry-run en RX-only hebben verschillende imagemarkers.

## Bestaande rollback en herkomst

`rollback` selecteert uitsluitend de al aanwezige RX-only firmware van
`d5c39e1b4d1f0ed37defe69b51e3562c36058208` in `.flashtools/fw/<commit>/`.
Bij een ontbrekend image wordt afgebroken. SHA-256-pins staan los van het
pakketmanifest in `tools/check_flash_package.py` en zijn bij de onafhankelijke
review vergeleken met het vooraf bestaande lokale hardwareverslag, flashlog,
ESP-imagehash en commitstempel. De historische broncontrole wordt ook lokaal
uitgevoerd. Er wordt geen historische Actions-status opgevraagd.

| Bestand | SHA-256 |
| --- | --- |
| bootloader.bin | `ef4ea64b8c5175a23427d78ce4c635a2d8d1b6811750c2597c46c5d458d2494a` |
| partitions.bin | `1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c` |
| boot_app0.bin | `f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0` |
| firmware.bin (historisch RX-only) | `6939546a631e76d94b684ed3964eacd37e6b8fa3897a1863d43232e30618a7db` |

Een lokale herbouw van de historische bron produceerde dezelfde bootloader en
tabel, maar een andere applicatiehash. Dit is lokale herkomstbevestiging van een
bewaard image, geen ondertekende supply-chain-attestatie of bit-reproduceerbare
herbouw. De eerdere hardwarelogs zijn bewijs van toen; de actuele bordinhoud
wordt pas na toestemming opnieuw gecontroleerd. Bewaar daarnaast de nieuw lokaal
gebouwde RX-only-variant en haar bewijsmap.

## Hardwaregrens: eerst afzonderlijke toestemming

**De onderstaande opdrachten zijn nu niet uitgevoerd.** Iedere precheck opent
de poort en reset via DTR/RTS naar de ROM-loader; ook een mislukte precheck kan
het bord daar achterlaten. Serial monitor, handmatige reset en USB-flash vallen
allemaal onder vooraf afzonderlijk toe te stemmen hardwaretoegang.

Na toestemming voor de specifieke release en deze handelingen:

1. Sluit andere seriële clients. Lees het etiket/model en de opgeslagen verwachte
   MAC uit de eigen lokale bordadministratie. Verander de MAC-pin niet om een
   afwijkende precheck te laten slagen. `tools/heltec_v2.sh ports` toont alleen
   bestandsnamen, zonder een poort te openen. Selecteer `PORT` expliciet:

   ```bash
   export PORT=/dev/cu.usbserial-0001  # vervang door de daadwerkelijk gekozen poort
   # .flashtools/board.mac moet de onafhankelijk bekende bord-MAC bevatten.
   LOCAL_ENV="$DRY" tools/heltec_v2.sh precheck "$SHA"
   ```

2. Precheck moet ESP32-D0WDQ6, verwachte MAC en precies 8 MB vinden. Hij leest en
   vergelijkt de **hele bootloader**, de exacte tabel en beide otadata-sectoren
   met de gepinde app0-selectie. Twee openbare eFuse-configuratiewoorden worden
   alleen gelezen; secure boot of actieve flash-encryptie blokkeert de procedure.
   Sleutelmateriaal en eFuse-wijzigingen worden niet benaderd. `boot_app0` bevat seq=1 met CRC `0x4743989a`,
   dat app0 selecteert. Elke afwijking stopt de procedure. Er is geen automatische
   terugval naar een ruimere flashprocedure, andere bootselectie of erase-all.

3. Wanneer ook de flash/reset expliciet is toegestaan:

   ```bash
   LOCAL_ENV="$DRY" tools/heltec_v2.sh flash "$SHA"
   ```

   Dit herhaalt precheck en bevriest de gecontroleerde images in een tijdelijke
   map. Typ alleen `yes` na controle van SHA, omgeving en poort. Uitsluitend
   `firmware.bin` op `0x10000` wordt geschreven, met `--after no_reset`.
   Vervolgens worden precies de appbytes teruggelezen en byte voor byte vergeleken;
   de SHA-256 wordt getoond. Alleen bij identieke readback volgt een normale reset.
   Bij schrijf-/leesfout of afwijking wordt gestopt zonder normale bootopdracht.

4. Observeer WiFi op het bestaande netwerk; verander geen identiteit of pairing.
   Het OLED moet de bootsleutel maximaal 60 s tonen terwijl het wakker is.
   Bij opgeslagen OLED Off/Auto kan eerst een korte PRG-druk nodig zijn; die
   fysieke handeling valt onder de afgesproken hardwaretest. Open de webinterface
   op het bestaande lokale IP-adres, noteer de volledige firmware-SHA en login
   met de sleutel. Controleer dry-run/no-RF, bron/unit, WiFi, OLED en ontvangst.
   De sessie vervalt na 10 minuten; geen ongeautoriseerde reset om haar te vernieuwen.

5. Voor een apart toegestane seriële ontvangstmeting:

   ```bash
   tools/heltec_v2.sh monitor 120 build/codex-hardware-boot.log
   ```

   Deze opdracht bevat geen aangevraagde reset, maar het openen van een seriële
   poort kan bordafhankelijk toch resetten. Verwacht `fw=<SHA>`, SX1276 TX disabled,
   `op` laagste bits 5, oplopende ontvangsttellers en geen UART-overflow,
   queue-drops, watchdog/panic of onverklaarde recovery. Gebruik bestaande
   passieve remote/unitberichten. Een bewuste druk op de fysieke remote zendt RF
   uit vanaf die remote en hoort alleen in een afzonderlijk toegestane test.

6. Na geldige bestaande remote- en unitobservaties en meer dan 3 s rust kan een
   stand die afwijkt van de gemelde unitstand **gesimuleerd** worden. Verwacht
   `dry-run-complete`, 50 framebytes, `rf_sent=false`, `unit_confirmed=false`,
   en blijvende RX. Vergelijk counters vóór/na; de ventilator hoort door de
   simulatie niet te veranderen. Test verboden legacy-routes op HTTP 403.
   In dry-run bestaan `/api/state`, configuratie, OTA en MQTT-bediening niet.

## NVS, stroomuitval en terugzetten

De exact gecontroleerde 8 MB-indeling:

| Partitie | Begin | Grootte |
| --- | --- | --- |
| nvs | `0x9000` | `0x5000` |
| otadata | `0xE000` | `0x2000` |
| app0 | `0x10000` | `0x330000` |
| app1 | `0x340000` | `0x330000` |
| spiffs | `0x670000` | `0x180000` |
| coredump | `0x7F0000` | `0x10000` |

De maximale appwrite blijft inclusief afronding op 4 KiB-wissectoren binnen app0.
Er is geen schrijf-/wisoverlap met NVS `0x9000–0xDFFF`, bootloader, tabel, otadata
of app1. De firmware genereert geen PoC-identiteiten en pairt/clonet niet.
WiFi en expliciete OLED-instellingen kunnen bij normaal gebruik wel legitiem
NVS bijwerken; byte-identieke NVS na opstart is dus geen passende garantie.
Bestaande NVS-backups blijven lokaal en worden niet automatisch teruggeschreven.

Een stroomonderbreking kan app0 gedeeltelijk wissen/schrijven. De bootloader en
ROM-downloadroute worden door deze procedure niet beschreven, maar automatische
terugkeer naar RX-only, continuïteit en gegarandeerde herstelbaarheid zijn niet
bewezen. App1 wordt niet als geteste fallback geclaimd. Stop bij fouten; voer geen
resetlus, erase-all, `--force`, eFuse-wijziging of partitieaanpassing uit.

Met afzonderlijke toestemming voor USB-herstel:

```bash
env -u LOCAL_ENV tools/heltec_v2.sh plan rollback       # offline, mag vooraf
env -u LOCAL_ENV tools/heltec_v2.sh precheck rollback   # opent/reset het bord
env -u LOCAL_ENV tools/heltec_v2.sh flash rollback      # schrijft alleen historische RX-app
# Alternatief: de met dezelfde herstelcommit geteste lokale RX-only-build:
LOCAL_ENV="$RX" tools/heltec_v2.sh flash "$SHA"
```

Controleer daarna WiFi, OLED, gekozen identiteit/unit en passieve RX. Bij
onbereikbare ROM-loader of afwijkende bootselectie stopt dit script. Handmatig
BOOT/PRG/reset-herstel vereist een afzonderlijke beoordeling en toestemming.

De keuze voor app-only, exacte bootselectie en grenzen volgt uit de lokale
boot_app0-bytes en Espressifs primaire documentatie:
[OTA bootselectie en rollback](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-reference/system/ota.html)
en [esptool, sectorwissen en imagechecks](https://docs.espressif.com/projects/esptool/en/release-v4/esp32/esptool/basic-commands.html).
