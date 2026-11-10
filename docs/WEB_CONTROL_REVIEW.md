# Orcon 15RF — overname, webbediening en profielen

Datum: 10 november 2026. Branch: `feature/setup-wizard-api`.
Basiscommit: `d9c3b5b44f3d18d108a1862433882f86c93587ff`.
Alle aanpassingen staan in de bestaande werkboom; er is niets gecommit, gepusht,
gemerged of geflasht. Er zijn geen GitHub Actions of fysieke RF-tests uitgevoerd.

## Overgenomen werk en eerste reparaties

Bij aanvang: **22 gewijzigde bestanden**, 1196 toevoegingen en 457 verwijderingen.
Overgenomen: SX1276-packet-TX, capabilities, PlatformIO-varianten, vijf standen,
transacties voor binding en standen, setup-API, compacte webpagina en host-tests.
De eerdere reparaties voor preprocessor versus C++-constanten en de volgorde van
TX-resultaten vóór `Pair::tick()` stonden al in de werkboom.

De suite stopte aanvankelijk op een ontbrekende capability-definitie in de test.
Na herstel bleven drie assertions over. De testklok liet een nog niet verwerkt
verzoek twee seconden wachten, voorbij de 500 ms-wachtrijgrens. De controller
weigerde dat terecht en blokkeerde vervolgaanvragen na de owner-time-out.
De test laat de owner nu werken voordat de klok verdergaat; de time-outbeveiliging
is behouden. De oorspronkelijke verwachte aantallen uit de overdracht zijn dus
geen beschrijving van de werkelijk aangetroffen testuitvoer.

## Technische reparaties

- De nieuwe TX-functies stonden in `radio::radio::sx1276`; de linkerfouten zijn
  opgelost door de correcte namespace te gebruiken.
- FSK-preamble-registers zijn **0x25/0x26**. De eerder gebruikte LoRa-aliases
  0x20/0x21 zijn in FSK RX-time-outregisters. De driver gebruikt nu de juiste adressen.
- Continuous RX wordt expliciet naar packet mode omgezet. Vaste lengte, NRZ,
  geen hardware-CRC, geen whitening of adresveld, sync uit en expliciet 10 dBm.
  Configuratiefouten en onjuiste register-readback verhinderen de TX-start.
- Het FSK-FIFO is maximaal 64 bytes zonder refill. Te lange frames worden geweigerd.
  De bestaande 24-byte legacy bind-offer zou gecodeerd ruim boven die grens uitkomen.
- De driver gebruikt PacketSent met de juiste RadioLib-constante. Een ongeldige
  IRQ-uitlezing `0xFF` telt niet als voltooiing. `rf_sent` betekent dat de radio een
  voltooid frame rapporteerde; het bewijst geen ontvangst door de ventilatie-unit.
- RX-herstel bewaart en herstelt gewijzigde registers en keert terug naar direct RX,
  ook bij fouten en time-outs. Volledige radio-reset/calibratie blijft bij de
  bestaande begrensde recovery. UART-echo en onvolledige decoderdata worden gewist;
  RX-tellers en de al ontvangen framequeue blijven behouden.
- TX-resultaten worden vóór het uitlezen van de RX-queue verwerkt. Een snel antwoord
  op een offer kan daardoor niet verloren gaan doordat `bind_sent` nog niet gezet was.
- Een dry-run-offer sluit af als `dry_run_complete`, zonder `bind_sent`, RF-bevestiging,
  slot-ACK of opgeslagen koppeling. Het houdt de sessie niet bezet.
- Een passend `31D9` wordt **waargenomen stand**, geen bevestiging van de TX-transactie.
  Een ongewijzigde of ambigue stand blijft afzonderlijk zichtbaar. Het observatievenster,
  tijdstempels, interferentiecontrole en gereserveerde transactie blijven behouden.
- Webcommando's hebben een verzoeknummer. Dubbele aanvragen met dezelfde inhoud
  geven hetzelfde resultaat; gewijzigde of verouderde aanvragen worden geweigerd.
  Eén transactie tegelijk, een rate floor, verse unitstatus en de remote-activity guard.
- Unit- en remotewaarnemingen worden niet meegenomen naar een ander doel-ID.
  Identiteitsconflicten blokkeren TX; de fysieke remote moet deze boot zijn gehoord.
- Mutaties vereisen JSON, een per-boot token en een passende Host/Origin. Web-control
  weigert mutaties via de setup-AP. Dubbele JSON-sleutels, trailing JSON, ongeldige
  numerieke waarden en onbekende velden worden geweigerd. De onbeschermde nieuwe
  OLED-mutatie is verwijderd; OLED-firmwarefunctionaliteit blijft behouden.
- De oude RF-PoC-build weigert de dry-run-webpagina en zendroute expliciet,
  zodat een dry-run-label nooit een echte uitzending kan verbergen. Alleen de
  aparte RF-web-control-build biedt de nieuwe RF-bediening.
- De bestaande RX-logroutes blijven beschikbaar. Een fysieke remote kan haar
  aangevraagde stand tonen zonder een antwoord van de unit als bewijs te gebruiken.
  Selectie van een remote start het uitlezen direct; technische IDs zijn secundair.

## RF-formaat en pairinggrens

De vijf mode-payloads voor tail `04` zijn `000004`, `000104`,
`000204`, `000304` en `000404`. Hardwarebewijs: zie *Onafhankelijke eindcontrole* hieronder. Tail `07` is alleen een afzonderlijk bekende
familievariant. Profielen gebruiken uitsluitend een consequent ontvangen `04` of `07`.

De bestaande codec verzorgt logische header/adressering/checksum, Manchester,
UART-framing, sync en stop. De acht softwaretrainingbytes volgen de bewezen
CC1101-UART-implementatie. Een modeframe levert **53 bytes / 424 bits**, inclusief
vier idle-padbits. Twee expliciete hardwaretrainingbytes geven nominaal 440 bits,
**11,46 ms bij 38,4 kbit/s**, exclusief mogelijke hardware/ramp-effecten.
Dit is een berekening en een registerconfiguratie, geen gemeten RF-resultaat.

Vergelijkingsbron: de lokaal gepinde RadioLib 7.8.1,
[implementatie van SX127x](https://github.com/jgromes/RadioLib/blob/7.8.1/src/modules/SX127x/SX127x.cpp)
en [registerdefinities](https://github.com/jgromes/RadioLib/blob/7.8.1/src/modules/SX127x/SX127x.h).
CC1101 en SX1276 delen de softwarecodec, maar hun modem-, FIFO- en GPIO-besturing
zijn afzonderlijk beoordeeld en niet onderling uitwisselbaar.

De capture ondersteunt de volgorde `I 1FC9 remote → broadcast`,
`W 1FC9 fan → remote`, `I 1FC9 remote → fan`. Het eerste offer is afgekort.
Er zijn geen ontbrekende bytes gereconstrueerd. De bestaande legacy-template
blijft alleen in de dry-run-simulatie bruikbaar; het is geen bewezen offer voor
het nieuwe doelapparaat. **Echte RF-pairing is daarom in de API/capabilities én
in de RF-backend geblokkeerd.** De complete W-tuple en slotboodschap zijn met
synthetische identiteiten getest. Strikte verwerking vereist het juiste type,
code, volledige tuple en overeenkomende afzenderidentiteit.

De vijf RF-standcommando's vereisen een afzonderlijk gecontroleerde, reeds door
de unit geaccepteerde eigen zenderidentiteit. Een succesvolle build bewijst die
acceptatie niet. De RF-image genereert geen nieuwe zenderidentiteit op eerste boot.
De gewone Heltec V2-build blijft RX-only; de projectbrede bestaande V3-default
is niet gewijzigd. MQTT-integratie is niet uitgebreid.

## Profielen

Na alle zes ontvangen knoppen verschijnt **Profiel opslaan**, met een naamveld.
Het bedieningsscherm heeft een compacte selector, hernoemen en verwijderen.
De zes mappings zijn Woning, Auto, Timer, Stand 1, Stand 2 en Stand 3. Timer
blijft een ontvangen `22F3`-mapping en wordt niet als zesde TX-commando gereplayed.

- Maximaal **vier profielen**, één per ontvangen remote-identiteit; opnieuw uitlezen
  en opslaan van dezelfde remote werkt het bestaande profiel bij en behoudt het ID.
- Namen zijn maximaal **48 UTF-8-bytes**, gevalideerd en veilig JSON/HTML-geëscaped,
  en uniek (`profile-name-taken`): de naam kiest in de UI het RF-doel.
- Alleen een volledig afgeronde leessessie kan een profiel via de API opslaan.
  Identiteit en mappings komen van de firmware, niet uit aangeleverde raw frames.
- Volledige payloads tot acht bytes per knop worden bewaard. Afgekorte mappings
  worden niet opgeslagen. Bij een onbekende familie, ontbrekend doeladres of
  tegenstrijdige adressen kan een volledig ontvangen profiel worden bewaard,
  maar is bediening ermee geblokkeerd.
- Het doeladres wordt alleen afgeleid uit de zes remoteframes, onafhankelijk van
  unit-antwoorden. TX vereist daarnaast verse status en de bestaande veiligheidschecks.
- Profielen bewaren **geen pairingcredential** en vervangen nooit de eigen zender-ID.
  Opslaan, selecteren, hernoemen en verwijderen starten geen RF-transactie.
- De actieve selectie blijft na herstart behouden. Selectie verandert in V2 alleen
  de gemonitorde unit. Legacy TX-builds worden niet geretarget:
  hun periodieke Orcon-verkeer mag door profielselectie niet naar een nieuw doel gaan.
- De NVS-namespace is `orconprofiles`, key `data`: één atomair vervangen blob,
  expliciete bytecodering, versie en CRC32. Maximaal 526 bytes profieldata;
  padding of platformafhankelijke C++-struct-layout wordt niet opgeslagen.
- Schrijffouten behouden de vorige actieve data in RAM en geven geen succes.
  Corruptie, onbekende versies en leesfouten leiden tot onbeschikbare opslag;
  er wordt niets automatisch gewist. Web-TX valt daarbij niet stilzwijgend terug
  op een andere unit. Identieke updates schrijven niet opnieuw naar NVS.

API, met dezelfde POST-beveiliging als de wizard:

| Methode | Route | Inhoud |
| --- | --- | --- |
| GET | `/api/profiles` | opslagstatus, limiet, actieve selectie, namen en eigenschappen |
| POST | `/api/profiles/save` | `sid`, `name` van de afgeronde uitleessessie |
| POST | `/api/profiles/select` | `id`; nul kiest de oorspronkelijke geconfigureerde unit (alleen RX-monitoring) |
| POST | `/api/profiles/rename` | `id`, `name` |
| POST | `/api/profiles/delete` | `id`; verwijderen van het actieve profiel wist ook de selectie |

Profielmutaties zijn geblokkeerd tijdens uitlezen, pairing of een open TX-transactie.
De bestaande webserver buffert HTTP-bodies vóór de handler en kan op een trage LAN-client
wachten. De RF-owner blijft gescheiden. Het token is CSRF-bescherming, geen gebruikerslogin;
HTTP is ongecodeerd. Deze firmware hoort op een vertrouwd, afgeschermd lokaal netwerk.

## Nog benodigde hardwarevalidatie

1. Een afzonderlijk goedgekeurde flash/boot-test, behoud van NVS en een RX-only rollback.
2. Werkelijke profielopslag, actieve selectie na stroomonderbreking, NVS-vol/errorpad,
   en controle dat beheer nooit een RF-uitzending veroorzaakt.
3. Web/OLED-bediening op ESP32, vrij heap, stackhoogwater en timing onder WiFi-belasting.
4. Eén afzonderlijk goedgekeurd standcommando met een reeds geaccepteerde identiteit,
   onafhankelijke RF-capture, controle van adres/payload/checksum/framegrens, training,
   bitrate, frequentie, afwijking, spectrum, zendvermogen en werkelijke duur.
5. Terugkeer naar RX na succes, ontbrekend antwoord en geïnjecteerde fout/time-out;
   geen eigen echo als identiteitsconflict en behoud van fysieke remotemonitoring.
6. Acceptatie van alle vijf standen door de unit; een ontvangen standmelding blijft
   observatie en geen sluitend ontvangstbewijs van ons commando.
7. Een volledige, onafgekorte 1FC9-offer en aparte pairingvalidatie voordat echte
   pairing vrijgegeven kan worden. Een opgeslagen profiel vervangt deze stap niet.

## Oordeel

| Onderdeel | Oordeel | Betekenis |
| --- | --- | --- |
| Dry-run-webbediening | GO voor lokale softwaretests | geen uitzendingen of veronderstelde bevestiging |
| Profielopslag | CONDITIONAL GO voor firmwaregebruik | lokaal getest; echte NVS/reboot/power-loss-test ontbreekt |
| Profielselectie | CONDITIONAL GO voor firmwaregebruik | lokaal getest; herstel op ESP32 nog te verifiëren |
| Echte RF-webbediening | NO-GO voor dagelijks gebruik | geen bewezen SX1276-on-air- of unitacceptatietest |
| Eén gecontroleerde fysieke TX-test | CONDITIONAL GO | alleen met aparte toestemming, vertrouwd LAN, vooraf gecontroleerde reeds gekoppelde eigen identiteit, onafhankelijke meting en rollback; geen pairing |
| Merge naar main | NO-GO | klaar voor code-review als experimentele wijziging; hardware- en pairingvrijgave ontbreken |

Lokale verificatie en firmwaregroottes staan hieronder. ASan is geen PASS: de
runtime blijft vóór de eerste test hangen; ook een leeg `main()` loopt vast.
UBSan en gewone host-tests worden afzonderlijk gerapporteerd.

## Lokale testresultaten

De volledige `test/run_host_tests.sh` en dezelfde suite met
`CXXFLAGS='-fsanitize=undefined -fno-sanitize-recover=all'` slagen allebei.
Geen van deze tests opent echte hardware.

| Suite | Resultaat gewone run en UBSan |
| --- | --- |
| codec | 34 checks, 0 failures |
| frames | 35 checks, 0 failures |
| rx | 17 checks, 0 failures |
| log | 12 checks, 0 failures |
| orcon_rx | 21 checks, 0 failures |
| orcon_v3 | 12 checks, 0 failures |
| rxpipe | 15 checks, 0 failures |
| netcfg | 8 checks, 0 failures |
| oled | ALL PASSED, 0 failures |
| tx_poc | 557 checks, 0 failures |
| profiles | 50 checks, 0 failures |
| setup | 110 checks, 0 failures |
| setup_web_rx | 67 checks, 0 failures |
| setup_web_tx | 96 checks, 0 failures |
| setup_web_webctl | 112 checks, 0 failures |
| setup_web_webctl_rf | 106 checks, 0 failures |
| SX1276-registers en herstelmodel | 97 checks, 0 failures |
| setup-pagina | 48 checks, 0 failures |
| gegenereerde gzip-header | identiek aan de HTML-bron |

Aanvullende regressies:

- `tools/test_tx_poc_safety.py`: PASS; bestaande RX-only/OLED-guards lokaal uitgevoerd,
  guardmutaties, drie geldige capability-configuraties en tien verwachte compile-weigeringen.
- `tools/test_app1.py`: 19 tests, OK.
- `tools/test_rxmon.py`: 19 checks, 0 failures, met de bestaande Python-venv/pyserial.
- `tools/test_rxmon_soak.py`: 19 tests, OK, op synthetische pseudo-seriële data en localhost.
  Dit is geen fysieke duurtest. De sandboxbeperking op localhost en de ontbrekende
  pyserial in de algemene Python-omgeving waren geen firmwarefouten; de suite slaagt
  met de bestaande venv en toegestane lokale testserver.
- Gesimuleerde flash-precheck: 66 checks, 0 failures. Gesimuleerde lokale flashflow:
  66 checks, 0 failures. Er zijn geen echte seriële apparaten geopend of beschreven.
- ELF-controle van RX-only, TX-PoC-dry-run en web-control-dry-run: PASS.
  Geen bereikbare TX in de opgeloste directe aanroepgraaf; dry-run gebruikt alleen
  `DryRunBackend`. De bestaande self-test detecteert een geïnjecteerde fout.
  Register-indirecte virtuele aanroepen zijn een expliciete grens van deze analyse.
- Acht uitgegeven paginascripts: syntax OK. OLED-hoofdpagina, OLED-setup en oude
  PoC-webpagina: gedragstests PASS. De nieuwe wizard heeft daarnaast haar eigen
  48 controles hierboven.
- `git diff --check`: schoon. Privacycontrole omvat ook de nieuwe bron-, test- en
  documentbestanden; echte capture-identiteiten zijn niet als fixtures toegevoegd.
- AddressSanitizer: **NOT TESTED / runtime unavailable**. De eerste codec-test
  time-out na 60 seconden; ook een leeg `main()` start niet binnen acht seconden.
  Er zijn geen beveiligings- of quarantine-instellingen aangepast om dit te omzeilen.

Ruwe lokale resultaten: `build/takeover/host.log`, `ubsan.log`, `build-verified.log`,
`pages.json`, de afzonderlijke toolinglogs en `source-hashes.json`. Deze directory
is genegeerd door Git en bevat geen nieuw flashpakket of uploadopdracht.

## Onafhankelijke eindcontrole (Claude, 10 november 2026)

Gecontroleerd: dezelfde ongecommitte werkboom op `d9c3b5b`. Gevonden en hersteld:

1. **Web-TX raadde de unitfamilie.** Zonder actief profiel gebruikte `/api/remote/send`
   de geleerde tail en viel bij onbekend terug op `04`. Ook Woning→`00` en Auto→`04`
   waren dan niet per afstandsbediening bewezen. Nu geldt in web-control-builds:
   TX alleen met een actief profiel met volledige mapping (`profile-required`,
   `profile-protocol-unsupported`), en de profielunit moet het TX-doel zijn
   (`profile-unit-mismatch`). `submitCtl` weigert elke tail behalve `04`/`07`.
   De geleerde-tail-NVS-sleutel `tail` en `orcon::remoteTail()` zijn verwijderd.
2. **Dubbele profielnamen** werden geaccepteerd; nu `409 profile-name-taken`.
3. **Pagina**: in web-control blijven de knoppen uit zonder bruikbaar actief profiel.
   Een null-dereferentie vóór de eerste statusrespons is ook hersteld.

Hardwarebewijs van de gebruiker (lokale, niet-gecommitte `pairing.log` en een
schermafbeelding van de uitleeswizard; IDs gemaskeerd):

- Eigen afstandsbediening → eigen unit: `22F1` `000004`, `000104`, `000204`, `000304`,
  `000404`. De unit meldt daarna 31D9 met `00`, `01`, `02`, `03` en `04`.
  De Woning-knop zendt dus mode `00`. Timer: `22F3 00020F03040000` (7 bytes).
- Koppelhandshake: `I 1FC9 remote→broadcast`, `W 1FC9 unit→remote` met payload
  `00 31D9 <unit-ID>` (6 bytes), `I 1FC9 remote→unit` met `00`. De W- en slotframes
  komen overeen met de strikte verwerking. **Het offer is in de console afgekapt
  op 6 bytes**; het volledige offer ontbreekt nog, dus pairing blijft geblokkeerd.
- De unit is klasse 29, niet 32.
