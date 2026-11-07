# Heltec V2 (SX1276) — RX-betrouwbaarheid RAMSES-II / Orcon MVS-15RHBP

Branch `feature/heltec-v2-rx-only`. Firmware is RX-only: `radio::sendFrame()` is in
de SX1276-backend geblokkeerd, `orcon::tick()` is uit, en CI weigert elke TX-call
in die backend.

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

`[rxstat] uart ovf sync5 sync4 frames resets pend op lna rxcfg rxbw rssi heap`

- `sync5`: exacte `FF 00 33 55 53` (de oude decoder). `sync4`: `00 33 55 53` (de nieuwe).
- `frames`: frames die Manchester + checksum doorstaan.
- `ovf`: UART-overflows (verloren bytes).
- Per sync: `[sync <ms> <dBm> pre=XX]`, met XX = het byte vóór `00` (FF = uitgelijnd; 55/D5/F5/FD = verschoven UART-fase). Een sync zonder `[rx]`-regel binnen 500 ms is een verminkt frame.
- `op`: RegOpMode, laagste 3 bits `5` = RX. `lna`: bits 7-5 = gebruikte LNA-gain.
- Per frame: `[rx <ms> <dBm>] I --- 29:… 22F1 003 …`.

## Flashen (macOS, zonder PlatformIO)

```
tools/heltec_v2.sh precheck <commit>   # alleen lezen
tools/heltec_v2.sh flash    <commit>   # precheck, dan "yes", dan schrijven
```

`precheck` stopt bij elke afwijking, ontbrekende waarde of fout. Er wordt niets geschreven.

- **Artifact:** SHA256SUMS, `BUILD_INFO` (commit en env), commit-stempel en RX-only-strings in `firmware.bin`, image-header Chip ID 0 (ESP32) met geldige hash, 8 MB in bootloader- en app-header, CI-status.
- **Bord (alleen lezen):** `Chip is ESP32-D0WD…` en exact `Detected flash size: 8MB`.
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
