# Heltec V2 (SX1276) — RX-betrouwbaarheid RAMSES-II / Orcon MVS-15RHBP

Branch `feature/heltec-v2-rx-only`.

## RX-only: wat afgedwongen wordt (`RX_ONLY`, `config.h`)

`RX_ONLY` staat automatisch aan voor `RADIO_SX1276`. Een SX1276-build zonder die vlag
compileert niet (`#error`).

| Laag | Afdwinging |
|---|---|
| Radio | SX1276-backend heeft geen TX-pad; `sendFrame()` weigert; CI weigert `transmit`/`startTransmit`/`transmitDirect` in de backend |
| Logica (`orcon.cpp`) | elke TX-, pairing-, keeper- en selftestfunctie weigert vóór enige toestandswijziging (`RX_ONLY_REFUSE`); `tx()` registreert alleen echt verzonden frames; `tick()` doet niets |
| API / MQTT | `/api/cmd` en MQTT: elke sleutel buiten `maskids`/`showfilter`/`roomidx` → hele verzoek geweigerd (403 `{"err":"rx-only"}`), geen gedeeltelijke toepassing, geen NVS-schrijfactie |
| Netwerkupdate | ArduinoOTA en `/update` zijn weggecompileerd; CI controleert dat het image die strings niet bevat |
| MQTT | eigen namespace `orcon15rf_rx/…` en HA-node `orcon15rf_rx` (overschrijft nooit een productiebord), geen command-topic, geen HA-bedieningsentiteiten |
| Zichtbaarheid | state-JSON `rx_only:true`, `fw`, `tx_refused`, `cmd_rejected`; elke webpagina toont een RX-ONLY-banner met uitgeschakelde knoppen |

Beperkingen: de webpagina's tonen nog de (uitgeschakelde) bedieningsknoppen. Er is geen authenticatie op de web-API. Een LAN-gebruiker kan UI-instellingen en `/api/ids`/`/api/wifi` wijzigen, maar kan niet zenden en geen firmware uploaden. USB-flashen blijft altijd mogelijk.

## Ontvangst onder netwerkbelasting

- Na `begin()` bezit één FreeRTOS-taak (`ramses_rx`) de SX1276. Die taak heeft prioriteit 3 en draait op de core van de loop, met een periode van 5 ms en task-watchdog. Alleen deze taak raakt SPI en de RX-UART aan.
- De loop (web, MQTT, WiFi) haalt alleen gedecodeerde frames uit `FrameQueue<32>`. Die queue is lock-free met één producent en één consument. Is hij vol, dan valt het **nieuwste** frame weg en wordt het geteld (`rf_queue_drops`). Gedeelde toestand (`orcon`, `rlog`) wordt alleen in de loop gewijzigd.
- De taak controleert RegOpMode elke seconde. Staat de radio niet in RX, of mislukt de init, dan volgt een begrensde herstart: maximaal 10 per boot, met backoff 1, 2, 4, 8, 16 en 30 s. Daarna blijft `rf_state` op `failed` staan.

Nieuwe meetwaarden (`[rxstat]` en `/api/state`):

| Veld | Betekenis |
|---|---|
| `pendmax` | Maximale bezetting van de 8 KB UART-buffer |
| `q`, `qmax`, `drop` | Queuediepte, high-watermark en drops |
| `taskus` | Langste doorloop van de RX-taak |
| `stack` | Ongebruikte stack van de RX-taak |
| `minheap` | Minimale vrije heap |
| `loopmax` | Langste loop-iteratie, oftewel de langste netwerkstall |
| `rxst`, `err`, `rec`, `lost` | Herstelstatus van de radio |

Model (`test_rxpipe`), géén hardware: bij een DNS-storing van 5 s, trage HTTP van 3 s ×3 of een WiFi-reconnect van 15 s verloor de oude één-loop-opzet 3, 3 en 13 frames. De nieuwe opzet verliest er geen.

## Unitselectie

- **Unitstatus** komt alleen van de **expliciet gekozen** unit: `fan_id`, opgeslagen in NVS en in te stellen via `/api/ids`. In RX-only wordt nooit automatisch een unit gekozen; zonder gekozen unit is er geen unitstatus.
- **Een 22F1 van een afstandsbediening aan die unit** wordt apart bewaard in `remote_cmd`/`remote_id`/`remote_age`. `remote_confirmed` wordt pas waar als de unit zelf binnen 10 s die stand meldt. `mode` en `mode_age` komen alleen van de unit.
- **`/api/log` → `devices`** toont alle gehoorde afzenders met hun rol (op basis van berichten), aantal, leeftijd en RSSI. Dat is alleen discovery en kiest nooit iets.

## Uitgangsmeting (firmware d82ffa3)

Raw UART-bytes 105.873 → 183.618, sync markers 0 → 1, decoded RX 0 → 1, buffer
resets 0. De fysieke afstandsbediening stuurt de unit wel betrouwbaar.

## Oorzaken

| # | Oorzaak | Status |
|---|---------|--------|
| H0 | `g_radio.getRSSI()` (RadioLib 7.8.1, FSK, standaardargumenten) doet `startReceive()` + `standby()`. Na het **eerste** gedecodeerde frame stond de SX1276 in standby: geen RX meer tot een reboot. Verklaart "decoded 0 → 1". | Bewezen in de RadioLib-bron (`SX127x::getRSSICommon`). Gefixt in f0b15f1 (`getRSSI(false, true)`). Hardware: `op` moet in elke `[rxstat]` `x5` blijven. |
| H1 | De UART lockt op de alternerende 0x55-preambule op een willekeurige even bitfase. In 4 van 5 fases gaat de `0xFF` van `FF 00 33 55 53` verloren (het byte vóór `00` wordt `55/D5/F5/FD`). De decoder eiste alle 5 bytes. | Bewezen met test (`test_rx`, bit-level UART-model): oud 109/400, nieuw 400/400. Hardware: verhouding `sync5/sync4` ≈ 0,2–0,3 verwacht. |
| H2 | Na een verminkt frame sprong de decoder naar de eerstvolgende `0x35` in de buffer. Daardoor ging het frame daarna verloren (bijv. de 31D9 van de unit na een 22F1). | Bewezen met test: oud 2/3, nieuw 3/3 frames. |
| H3 | De rol van een apparaat werd uit de class afgeleid (fan = 32). Bij de MVS-15RHBP zijn unit én afstandsbediening class 29. | Bewezen met test op de echte frames. Gefixt in b259e89. |
| H4 | `RF_RXBW_KHZ = 250` op SX1276 is **single-sideband** (500 kHz totaal). Nodig is ≈ fdev + BR/2 = 70 kHz SSB plus kristalafwijking. evofw3/CC1101 gebruikt 270 kHz totaal (≈ 135 SSB). Ongeveer 3 dB extra ruis. | Hypothese, **niet gewijzigd**. Pas meten na H0–H2. |
| H5 | `directMode()` zet RxTrigger = none. De LNA-gain blijft daardoor statisch; sterke signalen van dichtbij kunnen de ontvanger verzadigen. | Hypothese, niet gewijzigd. `lna` en `rssi` worden gelogd. |

Gecontroleerd en in orde:

- DIO2 → GPIO34 als UART-RX, 38400 8N1, NRZ, polariteit (een checksum-geldig frame is eerder ontvangen).
- De bit-synchronizer staat aan (RadioLib-default).
- De UART-event-task wist geen data (arduino-esp32 2.0.16).

## Adressering MVS-15RHBP

- `I 29:000101 → 29:000202 22F1 000104`: afstandsbediening → unit.
- `I 29:000202 … 31D9 000003`: statusbroadcast van de unit.
- Rol volgt uit het bericht: de afzender van I/RP 31D9/31DA is de unit; de afzender van
  22F1/22F3 is een afstandsbediening en de bestemming is de unit. Uit de HRC400-code
  (`DEV_FAN_CLASS 0x20`) mag niets worden aangenomen.

## Diagnose (serieel elke 10 s, ook in `/api/state`)

`[rxstat] uart ovf sync5 sync4 frames resets pend pendmax q qmax drop taskus stack rxst err rec lost op lna rxcfg rxbw rssi heap minheap loopmax`

- `sync5`: exacte `FF 00 33 55 53` (de oude decoder). `sync4`: `00 33 55 53` (de nieuwe).
- `frames`: frames die Manchester + checksum doorstaan.
- `ovf`: UART-overflows (verloren bytes).
- Per sync: `[sync <N> <ms> <dBm> pre=XX]`, met XX = het byte vóór `00` (FF = uitgelijnd; 55/D5/F5/FD = verschoven UART-fase). Elk frame: `[rx <N> <ms> <dBm>] …`, met hetzelfde syncnummer N. Een sync zonder `[rx]` met dat nummer is een verminkt frame. `tools/rxmon.py` koppelt ze exact.
- `op`: RegOpMode, laagste 3 bits `5` = RX. `lna`: bits 7-5 = gebruikte LNA-gain.
- Per frame: `[rx <ms> <dBm>] I --- 29:… 22F1 003 …`.

## Flashen (macOS, zonder PlatformIO)

```
tools/heltec_v2.sh precheck <commit>   # alleen lezen
tools/heltec_v2.sh flash    <commit>   # precheck, dan "yes", dan schrijven
```

`precheck` stopt bij elke afwijking, ontbrekende waarde of fout. Er wordt niets geschreven.

- **Artifact:** SHA256SUMS, `BUILD_INFO` (commit en env), commit-stempel en RX-only-strings in `firmware.bin`, image-header Chip ID 0 (ESP32) met geldige hash, 8 MB in bootloader- en app-header, CI-status.
- **Bord (alleen lezen):** `Chip is ESP32-D0WDQ6`, het MAC-adres gelijk aan `HELTEC_MAC` of `.flashtools/board.mac`, en exact `Detected flash size: 8MB`. Alle check-runs van de commit moeten `success` zijn.
- **Partitietabel:** de tabel van het bord (0x8000, 0xC00) moet byte-identiek zijn aan de nieuwe (NVS op 0x9000).
- **Schrijven:** daarna worden de 4 images op hun eigen offsets geschreven, zodat NVS (WiFi/MQTT/ID's) behouden blijft. Het merged image op 0x0 zou NVS wissen.

esptool 4.8.1 draait uit de PyPI-sdist (SHA-256 gepind) met hash-gepinde wheels in
`.flashtools/`. Er wordt niets systeembreed geïnstalleerd.

## Hardwaretestprocedure

Voorwaarde: geen ander programma (Opera/esptool-js, serial monitor) houdt
`/dev/cu.usbserial-0001` open.

1. **Flashen:** `tools/heltec_v2.sh flash <commit>`.
2. **Opstarten:** `tools/heltec_v2.sh monitor 40 boot.log reset`. Verwacht:
   - `fw=<volledige commit>`;
   - `[radio] SX1276 direct RX (TX disabled) up on 868.300 MHz`;
   - `[rxstat]` met `op=x5`.
3. **Rustmeting, 5 min, niet drukken:** `tools/heltec_v2.sh monitor 300 idle.log`.
   Noteer de delta's van `uart` en `ovf`, het RSSI-bereik en eventuele spontane 31D9/31DA van de unit.
4. **Druktest:** `tools/heltec_v2.sh monitor 330 press.log`.
   - Wacht 30 s.
   - Druk dan 20× op de afstandsbediening, om de 15 s, en wissel de stand af (1-2-3-1-…).
   - Noteer per druk de tijd.
5. **Beoordelen:**
   - `op` blijft overal `x5`, en er komen ook ná het eerste frame frames binnen → H0 gevalideerd.
   - Gedetecteerde drukken (22F1 van 29:000101) ≥ 19/20, met daarnaast de 31D9 van 29:000202.
   - `sync5/sync4` ≈ 0,2–0,3 → H1 gevalideerd (de oude decoder zou alleen `sync5` hebben gehaald).
   - `frames/sync4` ≥ 0,9 → bitfouten zijn geen probleem. Lager → RF-kwaliteit (stap 6).
   - `ovf` = 0. Is `ovf` > 0, dan is de loop te traag (WiFi/MQTT): dat is een softwarefix, geen RF.
6. **Volgende iteratie, alleen als stap 5 tekortschiet. Eén parameter per build:**
   - a. `RF_RXBW_KHZ` 250 → 125 (SSB). Herhaal stap 3–5 en vergelijk `frames/sync4`, de RSSI van verminkte syncs en het aantal drukken.
   - b. Alleen bij sterke signalen met fouten (`rssi` > −50 dBm): LNA/AGC-experiment.
7. **Terugrollen naar d82ffa3:** het artifact bestaat tot 2026-11-21 en heeft geen commit-stempel. Gebruik de partitiecontrole en
   `esptool --chip esp32 write_flash 0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin`
   uit `.flashtools/fw/d82ffa3…/`.

## Meetresultaten

### Meting 1: firmware 1379f6b, 2026-11-07 17:45–17:55, 600 s via USB

Flash: precheck OK (ESP32-D0WDQ6 rev 1.0, 8 MB, partitietabel identiek), 4×
"Hash of data verified", NVS niet gewist. De oude firmware draaide zonder
WiFi-config (SoftAP), dus er is alleen via USB gemeten.

| Meting | d82ffa3 (jouw meting) | 1379f6b |
|---|---|---|
| RX actief na het eerste frame | nee (standby) | ja, `op=0D` gedurende 600 s |
| sync5 (oude decoder) / sync4 | 1 / – | **0 / 6** |
| Gedecodeerde frames | 1 | 3 (20:000303 31D9 + 31DA, 37:000404 31DA; −73/−77 dBm) |
| UART-overflow / buffer-resets | – / 0 | 0 / 0 |
| Ruis op de UART | – | ±2,7 kB/s, RSSI-ruisvloer −87…−102 dBm |
| LNA / RxConfig / RxBw | – | G1 (max) / AgcAutoOn, trigger none / 250 kHz SSB |

- **H0 en H1 zijn op hardware bevestigd.** De oude decoder had geen van de 6 syncs gevonden.
- **Ongeveer de helft van de syncs gaf geen geldig frame.** Dat is nu de beperkende factor.
- **Niet bekend:** of er tijdens de meting op de afstandsbediening is gedrukt. Er kwam geen enkel frame van 29:000101 of 29:000202.
- **Twee monitorbevindingen:**
  - Het openen van de poort resette het bord (DTR/RTS-volgorde). Gefixt in `rxmon.py`.
  - `ferr` werkt niet: de ESP32-driver zet de frame-error-interrupt niet aan. De teller is verwijderd.

### Meting 2: firmware 7812177 (RX-logica gelijk aan 1379f6b + `[sync]`-diagnose), 18:21–18:33, 720 s

Zelfde precheck en flash als meting 1. Twee drukrondes (normale plek en ±1 m). De
monitor opende de poort zonder reset, zoals bedoeld.

| Meting | d82ffa3 | 1379f6b (meting 1) | 7812177 (meting 2) |
|---|---|---|---|
| RX na het eerste frame | nee | ja | ja, `op=0D` gedurende 710 s |
| syncs → frames | 1 → 1, daarna doof | 6–7 → 3 | **62 → 59** (95%) |
| sync5 (oude decoder) | 1 | 0 | 22 van 62 (35%) |
| 22F1 van 29:000101 (afstandsbediening) | – | 0 | **21 frames**, ≥ 17 gescheiden drukken, −27…−45 dBm |
| 31D9 van 29:000202 (unit) | – | 0 | 33 frames; 16 van 17 drukken binnen 300 ms beantwoord |
| Verminkte syncs | – | 3–4 | 3, allemaal zwak (−82…−88 dBm), geen tijdens het drukken |
| `ovf` / `resets` / LNA | – | 0 / 0 / G1 | 0 / 0 / G1 (constant) |

- Byte vóór `00`: 55 (19×), FF (22×), D5 (10×), F5 (7×), FD (4×). Dit zijn precies de vijf
  UART-fases die H1 voorspelt; daarmee is het mechanisme op hardware bevestigd.
- H5 (overmatig signaal) is **verworpen**: bij −27 dBm, met de LNA op maximale gain, worden alle frames foutloos gedecodeerd.
- Adressering bevestigd: 29:000101 → 29:000202 22F1 `00 0N 04` (N = stand 1–3). De unit antwoordt
  met 31D9 `00 00 0N`, direct en ±1 s later nog een keer. 20:000303 en 37:000404 sturen 31D9/31DA (17/30 bytes):
  andere afzuigunits (buren), −74…−81 dBm.
- Meting 1 ontving niets van de afstandsbediening, ook geen 31D9 van de unit, met dezelfde RX-logica. Waarschijnlijk vielen de drukken buiten het meetvenster.
- Diagnosebeperking: de RSSI wordt gemeten wanneer de loop de bytes verwerkt. Bij een korte
  stall krijgen twee frames dezelfde (latere) waarde; een −91 dBm bij een 22F1 is daar een voorbeeld van. Het decoderen zelf heeft hier geen last van.

**Conclusie:** geen RF-wijziging nodig voor eigen apparaten op deze afstanden. RxBw 250 → 125 kHz blijft
een optie voor zwakke signalen (< −80 dBm), maar alleen als dat ooit nodig blijkt.

## Duurtest 24–72 uur (WiFi + MQTT): criteria

**Vooraf (stop als één punt niet klopt)**
- Firmware = een commit waarvan **alle** CI-jobs groen zijn: V2-build, host-tests (gewoon, ASan/UBSan, TSan), V3/CC1101-, V3/SX1262- en 4848-build. Flashen alleen na expliciet akkoord, via `tools/heltec_v2.sh flash`.
- WiFi (STA) en MQTT zijn ingesteld. Unit 29:000202 is gekozen via `/api/ids`. Het MAC-adres van het bord staat in `.flashtools/board.mac`.
- Productietopic `orcon15rf/state` en de HA-entiteiten van het productiebord zijn vastgelegd. Na de test moeten ze ongewijzigd zijn.

**Tijdens de test vastleggen**
- **Serieel:** `tools/heltec_v2.sh monitor <s> duur.log` (regelgebufferd).
- **MQTT:** een abonnement op `orcon15rf_rx/state`.
- **Bewuste verstoringen**, met tijdstip noteren:
  - broker 30 min uit;
  - access point herstarten;
  - onoplosbare MQTT-host (DNS-fout);
  - trage HTTP-client op `/api/log`;
  - afstandsbedieningsdrukken op bekende tijden.

**Geslaagd als**
- **Geen reset.** De boot-banner verschijnt één keer, de uptime loopt door, en er zijn geen task-watchdog-meldingen of `[net] state JSON … skipped`.
- **De radio blijft in RX.** `op` heeft in elk monster laagste 3 bits = 5. `lost` blijft 0; bij herstel staat `rxst` daarna weer op 0 en blijft `rec` onder 10.
- **Geen verlies van UART-bytes of frames.**
  - `ovf = 0`, `drop = 0` en `resets = 0`, ook tijdens de verstoringen (terwijl `loopmax` hoog oploopt);
  - `pendmax` blijft onder 4096.
- **Geheugen is stabiel.**
  - `stack` blijft boven 1024 B;
  - `minheap` vertoont geen dalende trend van meer dan 10 KB per 24 uur.
- **Drukken komen door.** Minstens 95% van de drukken geeft een 22F1 van 29:000101 en een 31D9 van 29:000202. `remote_confirmed` klopt met de 31D9.
- **Buren beïnvloeden niets.** Buurunits (o.a. 20:000303, 37:000404) veranderen nooit `fan_id`, `mode` of de unitstatus.
- **Commando's worden geweigerd.** Een bewust commando via de API geeft 403 en verhoogt `cmd_rejected`, maar `tx_count` blijft 0. Op het productietopic verandert niets.

**Direct afbreken bij**
- elke aanwijzing voor TX;
- unitstatus die van een ander apparaat komt;
- `rf_state = failed`;
- een reboot-lus;
- wijzigingen aan de productietopics.

Niet te verifiëren zonder extra meetapparatuur: dat er werkelijk geen 868 MHz-energie wordt uitgezonden. Daarvoor is een SDR of spectrumanalyzer nodig. Het bewijs is nu broncode, CI-guards en het ontbreken van TX-paden in het image.
