# Heltec V2 — 24 uur passieve RX/WiFi-observatie

Deze procedure is bedoeld voor firmware `d5c39e1b4d1f0ed37defe69b51e3562c36058208`.
Start uitsluitend na expliciet akkoord van de gebruiker. De gecontroleerde flash
en de korte OLED-hardwaretest zijn afzonderlijke voorwaarden; hun verslag staat
lokaal in `.flashtools/hardware-d5c39e1/RESULTS.md` (niet in Git). Deze procedure
flasht of reset niets en verandert geen firmware of instellingen.

## Scope en criteria

Dit is de MQTT-uit-variant van de criteria in [HELTEC_V2_RX.md](HELTEC_V2_RX.md).
MQTT blijft uit. Tijdens de meting: geen broker-, DNS- of AP-verstoringen, geen WiFi-scan,
geen API-commando's, geen OLED-wake, geen NVS-schrijfverzoeken, geen andere seriële
monitors en geen druktest met de afstandsbediening.

Het rapport kent vier uitkomsten: **PASS / FAIL / INCOMPLETE / NOT_TESTED**.
Een geslaagde meting heet uitsluitend **PASS (passieve RX/WiFi-scope)**. NOT_TESTED
telt nooit als geslaagd. Ontbrekend bewijs wordt nooit stilzwijgend geslaagd.
Het rapport geeft drie oordelen:

- **RF-firmware**: run- en RF-groep plus buurisolatie;
- **WiFi/HTTP**: run- en HTTP-groep;
- **totaal**: alles, inclusief de fysieke OLED-review.

| Groep | Criterium | Automatische beoordeling |
|---|---|---|
| run | `completed_24h` | ≥ 86.400 s en normale voltooiing; elke stop, crash, signaal of finalize is FAIL |
| run | `tool_provenance` | Tooling gecommit (`dirty=false`) en SHA-256 van `rxmon.py`/`rxmon_soak.py` bij start gelijk aan die bij `prepare` |
| run | `serial_coverage` | Seriële `[rxstat]` dekt de hele duur, geen gat > 25 s (firmwareklok `t`) |
| run | `serial_data_integrity` | Geen beschadigde regels; rxmon-tellercrosscheck klopt |
| rf | `no_restart_watchdog_tx_markers` | Eén boot; geen ROM-, banner-, watchdog-, panic-, backtrace-, JSON-skipped- of TX-melding |
| rf | `serial_rx_and_no_loss` | `ovf`, `drop`, `resets`, `lost` en `rxst` = 0 in elk monster; laagste drie bits van `op` = 5 |
| rf | `radio_no_recovery` | Tellers `rec` en `rf_recoveries` veranderen niet tijdens de opname (zie hieronder) |
| rf | `memory` | Vrije RX-stack > 1024 B; daling van `minheap` en het verschil tussen de eerste en laatste uurmediaan van de vrije heap elk ≤ 10.240 B (serieel) |
| rf | `rx_status_observed` | Frameteller stijgt; minstens één 31D9/31DA van de geselecteerde unit; buurstatus apart geteld |
| rf | `state_invariants` | Elk geldig API-monster: `fw`, `rx_only=true`, `tx_n=0`, verliestellers 0, MQTT uit, `fan_id` ongewijzigd, `rf_state=receiving`, OLED `auto`/uit, `pendmax<4096`. Geen geldig monster in de laatste 60 s → INCOMPLETE |
| rf | `uptime_continuous` | API-uptime daalt nooit |
| http | `http_available` | Alle GET's geven HTTP 200 en geldige JSON binnen 3 s; één mislukte GET is FAIL |
| http | `http_coverage` | Geldige API-monsters over de hele duur; geen gat > 13 s |
| review | `neighbor_isolation` | Alleen PASS als er buurverkeer was **én** een gedocumenteerde menselijke tijdlijncontrole; anders INCOMPLETE |
| review | `physical_oled` | Alleen PASS met fysieke waarnemingen in `review.json` |
| not_tested | drukken ≥95% en `remote_confirmed`; HTTP 403 en geweigerde TX-commando's; MQTT en productietopics; bewuste netwerkverstoringen; fysieke OLED-wake; werkelijke RF-uitzending zonder SDR | Altijd NOT_TESTED in deze passieve meting |

**Radioherstel.** In deze meting is elk herstel FAIL. Het oordeel steunt op de
tellers `rec` (serieel) en `rf_recoveries` (API), niet op toevallige monsters van
`rxst` of `rf_state`. Zo is het niet afhankelijk van het moment van meten. Herstel
van vóór de start telt niet mee: de waarde bij de preflight is de basislijn.
Dit is strenger dan "rec < 10" uit het oorspronkelijke scenario.

**WiFi/HTTP.** De firmware heeft geen aparte WiFi-linkstatus of WiFi-RSSI in `/api/state`.
Bereikbaarheid volgt uit de HTTP-resultaten. Alle seriële WiFi- en netwerkmeldingen
worden bewaard.

- **Logging gaat altijd door.** Een HTTP-storing, kort of urenlang, stopt de seriële
  opname nooit. De HTTP-thread blijft elke 5 s pollen.
- **Uitvalvensters.** Elk venster komt met begin, einde en duur in het rapport.
  Een venster dat bij het einde nog open is, krijgt `open_at_end`.
- **Wat wel afbreekt.** Een geldig HTTP-antwoord met een kritieke overtreding breekt
  de opname af, net als de seriële afbreekregels.

## Opslag en voorbereiding

Alle ruwe data en echte adressen blijven in een private map **buiten de Git-tree en
buiten tijdelijke mappen**. macOS leegt `/private/tmp` bij een herstart; `prepare`
weigert daarom `/tmp`, `/private/tmp`, `/var/folders` en paden binnen de repository.

```sh
cd <repository>
PY="$PWD/.flashtools/venv/bin/python"
RX="$PWD/tools/rxmon.py"
RUN="$HOME/OrconSoak/<run-id>"        # bijvoorbeeld 20261108T1200Z-24h
"$PY" -B "$RX" soak prepare "$RUN" --url "http://<bord>/api/state" --fan-id "<unit>"
```

`prepare` start geen proces, opent geen poort en verstuurt geen HTTP-verzoek. Het doet het volgende:

- maakt de map met rechten 0700 (bestanden 0600, umask 077);
- legt in `config.json` de Git-HEAD, `dirty` en de SHA-256 van `rxmon.py` en `rxmon_soak.py` vast;
- schrijft `agent.plist`.

Een gebruikte map kan niet opnieuw starten; bereid voor elke run een nieuwe map voor.

**Schijfruimte:**

- **Bij start**: minstens **10 GiB vrij** (gecontroleerd door `prepare`, `start` en de collector zelf), anders NO-GO.
- **Ruwe data**: maximaal **512 MiB**.
- **Tijdens de opname**: minstens **1 GiB + 16 MiB** blijft vrij.

Bij het bereiken van een grens stopt de opname met FAIL. Er is geen rotatie die
eerder bewijs verwijdert. De 35 s-proef voorspelt circa 78 MiB per 24 uur.

| Bestand | Inhoud |
|---|---|
| `serial.raw` | Exacte UART-bytes |
| `serial.log` | UTF-8-regels met UTC-tijd |
| `state.jsonl` | Per GET: starttijd, status, responstijd, exacte body (base64), geparseerde state, fouttype |
| `events.jsonl` | Handmatige waarnemingen |
| `collector.err` | stderr van de collector, begrensd tot 64 KiB |
| `status.json`, `run.json`, `result.json`, `report.md`, `config.json`, `agent.plist` | Status, verloop, rapport en configuratie |

De maximale HTTP-body is 64 KiB. Een seriële regel langer dan 16 KiB breekt de opname af.
Een stroomuitval kan de laatste schrijfactie verliezen en maakt de test ongeldig.

## Zelfstandig draaien en starten

`agent.plist` draait de collector als `ProcessType` Standard, zonder throttling,
onder de GUI-`launchd` van de gebruiker en `/usr/bin/caffeinate -ims`. Er is geen
afhankelijkheid van een terminal of agentsessie. Automatisch herstarten staat uit,
zodat meetgaten niet verborgen blijven. De plist staat in de runmap en niet in
`~/Library/LaunchAgents`, dus hij start niet opnieuw na een herstart.

`caffeinate` houdt idle- en schijfslaap tegen, en op netvoeding ook systeemslaap. Het scherm van de Mac mag uit. Verder geldt voor de Mac:

- blijf ingelogd, laat de klep open en houd netvoeding en USB aangesloten;
- kies zelf geen Slaap, Herstart of Log uit.

**Geen automatische herstart van macOS tijdens de test.** Zet in Systeeminstellingen →
Algemeen → Software-update → Automatische updates de optie "Installeer
macOS-updates" uit. Dit kun je controleren met:

```sh
defaults read /Library/Preferences/com.apple.SoftwareUpdate AutomaticallyInstallMacOSUpdates   # moet 0 zijn
pmset -g sched                                                                                   # geen geplande herstart
```

`soak start` weigert zolang automatische installatie aan staat. Het weigert ook bij
minder dan 10 GiB vrij, en als de tooling niet gecommit of na `prepare` gewijzigd is.

HTTP gebruikt de macOS-`/usr/bin/curl`, met een totale time-out van 3 s, zonder
proxy, redirects of retries. Python-`urllib` gaf onder launchd lokaal `EHOSTUNREACH`.
De seriële ontvangst draait in een eigen thread en wacht nooit op HTTP.

**Onderstaand commando start echt 24 uur en mag pas na expliciet akkoord:**

```sh
"$PY" -B "$RX" soak start "$RUN" --approved
```

Voordat de collector de poort opent, gebeurt het volgende:

1. Hij controleert de hashes van de tooling en de vrije schijfruimte.
2. Hij doet een state-preflight via HTTP. Verkeerde firmware, MQTT aan, OLED aan, een verkeerde unit of een verkeerde RX-toestand breken af.
3. Daarna opent hij de poort met `rxmon.open_serial`: DTR en RTS staan vóór `open()` op true, en de toegang is exclusief. Er bestaat geen resetoptie.

## Tussentijds controleren en veilig stoppen

```sh
"$PY" -B "$RX" soak status "$RUN"
tail -n 3 "$RUN/serial.log"
du -sh "$RUN"; df -h "$RUN"
pmset -g assertions
LABEL=$("$PY" -c 'import json,sys; print(json.load(open(sys.argv[1]))["label"])' "$RUN/config.json")
launchctl print "gui/$(id -u)/$LABEL"
```

`status.json` vernieuwt ongeveer elke 5 s. Het bevat `http_consecutive_failures` en
`http_outage_since`. Een status ouder dan 15 s zonder `end_utc` in `run.json` is verdacht.
Open geen tweede seriële monitor.

Noteer aan het begin, tussendoor en aan het einde fysiek dat het OLED uit is.
Druk niet op PRG en klik niet op het schermicoon. Noteer ook elke onbedoelde
bediening, netwerkverandering of slaapstand:

```sh
"$PY" -B "$RX" soak note "$RUN" 'Fysiek gecontroleerd: OLED uit; geen bediening'
```

Veilig afbreken:

```sh
"$PY" -B "$RX" soak stop "$RUN"       # STOP-bestand; wacht maximaal 30 s
"$PY" -B "$RX" soak status "$RUN"     # end_utc en report.md aanwezig?
"$PY" -B "$RX" soak unload "$RUN"
```

Een afgebroken run kan nooit PASS krijgen. Bij een vastgelopen collector: stuur
SIGTERM naar de PID uit `run.json`; ook dan ontstaat een FAIL-rapport. Is de
collector verdwenen (crash, SIGKILL of stroomuitval) en staat er in `run.json` nog
`running`, dan:

```sh
"$PY" -B "$RX" soak finalize "$RUN"   # weigert zolang de collector nog draait
```

`finalize` legt de stopreden "collector died or was killed" vast en maakt een
FAIL-rapport. Daarna is `unload` mogelijk. Herstart nooit het bord om logging te stoppen.

**Automatisch afbreken** gebeurt bij elk van de volgende gebeurtenissen:

- TX, gewijzigde fanselectie of MQTT aan;
- onverwacht OLED aan of een moduswissel;
- radio buiten RX, een nieuw radioherstel of verliestellers boven 0;
- een reboot of watchdog;
- de stackgrens of de UART-pendinggrens;
- een seriële fout of meer dan 25 s geen rxstat;
- volle logs of te weinig vrije schijfruimte.

HTTP-fouten breken **niet** af. Ze worden gelogd, staan als uitvalvenster in het
rapport en maken het WiFi/HTTP-oordeel FAIL.

## Eindrapport en menselijke bewijscontrole

Na 86.400 s ontstaan `report.md` en `result.json` automatisch. `result.json` bevat:

- `verdict`, `rf_verdict` en `http_verdict`;
- alle checks per groep en de NOT_TESTED-redenen;
- de tooling-provenance;
- tellerbereiken, RX-berichten en HTTP-uitvalvensters met latenties;
- geheugenwaarden en de stderr van de collector;
- SHA-256-hashes van de ruwe bestanden.

De bestaande parser blijft apart bruikbaar, zonder hardwaretoegang:

```sh
"$PY" -B "$RX" --file "$RUN/serial.log"
```

Controleer lokaal de bron en tijdlijn van ontvangen unit- en buurberichten tegenover de API-monsters:

- `fan_id` moet gelijk blijven;
- alleen berichten van de gekozen unit mogen `mode`, fan-/mode-age of unitstatus verversen.

Afzonderlijke statuswisselingen binnen de API-periode van 5 s kunnen niet volledig
automatisch bewezen worden. Zonder buurverkeer is het oordeel INCOMPLETE. Leg de
uitkomst met concrete tijdstippen en waarnemingen vast in `$RUN/review.json`:

```json
{
  "neighbor_isolation": {"passed": true, "evidence": "Beschrijf echte bron-/tijdlijncontrole lokaal"},
  "physical_oled": {"passed": true, "evidence": "Verwijs naar fysieke begin-/tussen-/eindwaarnemingen in events.jsonl"}
}
```

Gebruik `false` bij een aangetoonde fout, en laat een criterium weg als het bewijs
ontbreekt. Zet dit bestand nooit in Git. Genereer daarna opnieuw:
`"$PY" -B "$RX" soak report "$RUN"`.

Archiveer de runmap na afloop op een eigen private locatie. Wijzig of verwijder de
tooling, de venv en de meetmap niet tijdens de opname.

## Toolingvalidatie

```sh
"$PY" -B tools/test_rxmon.py
"$PY" -B tools/test_rxmon_soak.py
python3 tools/check_privacy.py
```

De synthetische tests dekken onder meer:

- **Uitval en herstel**: HTTP-uitval van 15 s, 5 min en 6 uur, gevolgd door herstel. `serial.log` groeit tijdens de uitval en de opname eindigt `completed`. Ook een uitval die bij het einde nog open is.
- **Afbreken en crashes**: een echte collector die met SIGKILL wordt gestopt, gevolgd door `finalize`; SIGTERM; operator-stop.
- **Fouten tijdens de opname**: een seriële reset-marker en een radioherstel.
- **Grenzen en guards**: schijfruimte, loglimiet, gewijzigde tooling en weigering van tijdelijke en Git-locaties.
- **Rapportage**: tooling met lokale wijzigingen, ontbrekende seriële data, NOT_TESTED-regels en het PASS-label.
- **Rechten en privacy**: bestandsrechten (ook bij `soak note` met umask 022) en de lijninstelling van de poort zonder reset.

CI voert beide Python-suites en de privacycontrole uit. Hardwareproeven blijven beperkt tot korte passieve opnames.
