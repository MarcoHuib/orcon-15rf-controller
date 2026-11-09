# Heltec V2 — OLED-energiebesparing

De instelling staat in het webportaal op **WiFi, MQTT & display setup** (`/wifi`),
met een schermicoon direct op de **hoofdpagina** en de fysieke **PRG-knop** op
het bord als snelle bediening. Met **Apply display setting** wordt de keuze
direct toegepast en in NVS opgeslagen, zonder herstart. De WiFi/MQTT-knop
**Save & reboot** is hiervoor niet nodig.

| Stand | Gedrag |
|---|---|
| Auto (standaard) | Scherm aan bij opstarten; uit na 30 seconden. PRG-knop of het webicoon toont het scherm opnieuw 30 seconden. |
| On | Scherm blijft aan. PRG-knop verandert niets; het webicoon zet het scherm tijdelijk uit tot de volgende actie of herstart. |
| Off | Scherm direct uit, ook na opnieuw opstarten. PRG-knop of het webicoon geeft 30 seconden scherm ("peek"), daarna weer uit. |

Handmatige acties (PRG-knop, webicoon) veranderen nooit de opgeslagen stand en
overleven geen herstart. Een ontbrekende of ongeldige opgeslagen keuze gebruikt
Auto. Ontvangen berichten, OLED-refreshes en webverzoeken herstarten de timer
niet. De timer blijft correct bij het overlopen van `millis()`; een verlopen
timer blijft uit tot een expliciete actie of herstart.

## PRG-knop (GPIO0)

Een korte druk op de PRG-knop start het kijkvenster opnieuw (zie de tabel).
De knop hangt op **GPIO0**, de boot-strapping-pin van de ESP32. De firmware
gebruikt de pin uitsluitend als input met pull-up (`INPUT_PULLUP`, nooit
aangestuurd). Daardoor blijft de oorspronkelijke functie intact: de chip
boot normaal, en reset + ingedrukt houden van PRG selecteert nog steeds de
ROM-downloadmodus voor flashen. Er zit 30 ms ontrimpeling (debounce) op de
detektie; een lange druk telt één keer. Andere builds gebruiken de pin niet.

## Hoofdpagina-icoon

Op de hoofdpagina (`/`) staat rechtsboven in de kop een schermicoon, alleen
zichtbaar op de V2-build (`oled_available`). Tikken stuurt `{"toggle":1}` naar
`/api/oled`: het scherm gaat direct uit als het aan staat, en anders direct
aan (zelfde gedrag als de PRG-knop). Het icoon volgt `oled_on` uit
`/api/state` en is op RX-only-borden gewoon bruikbaar — het is een lokale
scherminstelling, geen commando.

De firmware gebruikt uitsluitend U8g2 `setPowerSave(1)` / `setPowerSave(0)` voor
de SSD1306 en verstuurt geen schermbuffers zolang het scherm uit staat. De ESP32,
SX1276-ontvangst, WiFi, MQTT (indien ingeschakeld) en webserver blijven actief.
Vext blijft ingeschakeld; er wordt geen ESP32-slaapstand gebruikt.
Zie de [U8g2-documentatie](https://github.com/olikraus/u8g2/wiki/u8g2reference#setpowersave).

De instelling geldt alleen voor `heltec_v2_sx1276`. De twee Heltec V3-builds en
de 4848-TFT-build behouden hun bestaande schermgedrag.

## API

`POST /api/oled` met exact één veld: `{"mode":"auto"}`, `{"mode":"on"}`,
`{"mode":"off"}` of `{"toggle":1}`. Antwoord: 200 met `{"ok":1}`; ongeldige
invoer geeft 400, een mislukte NVS-opslag 500. `mode` wijzigt en persist de
stand; opnieuw dezelfde modus toepassen schrijft niet naar NVS, maar start in
Auto wel een nieuw kijkvenster. `toggle` wisselt het scherm direct aan/uit
zonder de opgeslagen stand aan te passen.

`/api/state` bevat op V2 `oled_available:true`, `oled_mode` en `oled_on`.
Op de andere builds is `oled_available:false` en is `/api/oled` niet geregistreerd.
De instelling gebruikt een apart lokaal endpoint; de RX-only-allowlist van
`/api/cmd` en MQTT is ongewijzigd. TX, pairing en netwerkupdates blijven geblokkeerd.

## Controle

- Hosttests: `test/run_host_tests.sh` (inclusief grenswaarden, moduswissels,
  opgeslagen keuzes, herhaald toepassen, timer-overloop en handmatige
  wake/sleep-acties per stand).
- Webtests: compileer `test/dump_pages.cpp` zoals in CI en voer
  `dump-pages 5 | node test/test_oled_setup.js` en
  `dump-pages 0 | node test/test_oled_main.js` uit.
- Compileer alle vier firmwarebuilds zoals in de bestaande CI-workflow.
- Na afzonderlijk akkoord voor flashen: controleer Auto na opstarten en na Apply,
  On/Off en opslag over een herstart. Controleer een korte PRG-druk in elke
  stand (Auto: opnieuw 30 s; Off: 30 s peek; On: geen verandering) en een
  langere druk (één actie). Controleer dat reset + ingedrukt houden van PRG
  nog steeds de downloadmodus selecteert en flashen met `tools/heltec_v2.sh`
  ongewijzigd werkt. Controleer het hoofdpagina-icoon (aan/uit/directe reactie)
  en na het uitschakelen dat `/api/state` bereikbaar blijft, uptime doorloopt,
  frames blijven binnenkomen en TX-tellers nul blijven. Controleer ook de
  SoftAP-modus.

Stroomverbruik en fysiek schermgedrag vereisen een hardwaremeting; hosttests en
builds meten deze niet.

## Lokale sanitizer-toolchain (macOS)

Op macOS 26.6.2 met Apple Clang 17.0.0 hangt zelfs een leeg ASan-programma vóór
`main()`. Een stack-sample toont herhaalde ASan-initialisatie: het opvragen van de
dyld-cache roept `_Block_copy`/malloc aan, waarna dezelfde initialisatielock
opnieuw wordt genomen. Dit staat los van de OLED-code. Gewone tests en losse
UBSan werken wel.

LLVM heeft dit opgelost met
[`_dyld_get_dyld_header`](https://github.com/llvm/llvm-project/pull/182943);
gebruik voor lokale sanitizer-tests een toolchain met die fix, bijvoorbeeld de
officiële LLVM 22.1.3-release, zonder de systeemcompiler te vervangen:

```sh
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
CXX=/path/to/LLVM/bin/clang++ CXXFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" test/run_host_tests.sh
CXX=/path/to/LLVM/bin/clang++ CXXFLAGS="-fsanitize=thread" test/run_host_tests.sh
```

Met LLVM 22.1.3 en de expliciete macOS SDK voltooien het minimale
reproductieprogramma en alle negen hostsuites zowel met ASan+UBSan als met TSan.

De runner stopt elke testbinary na 60 seconden (instelbaar via
`HOST_TEST_TIMEOUT_SECONDS`) en meldt een timeout als fout. CI blijft alle suites
gewoon, met ASan+UBSan en met TSan uitvoeren op Ubuntu; er worden geen
sanitizer-tests overgeslagen.

Voor de volgende hardwarestappen blijft afzonderlijk akkoord nodig: eerst een
gecontroleerde flash en korte OLED-test, daarna de 24-uurs RX/WiFi-duurtest met
MQTT uit. Die stappen zijn niet onderdeel van de automatische CI.
