# Updatecontrole en éénknopsupdate (control-firmware, Heltec V2)

Status: gebouwd en host-getest op `feature/github-releases-control-ota`. **Niet op hardware getest** (TLS-handshake, geheugen tijdens de download, de echte proefstart). Werkt pas zodra er een officiële release op GitHub staat ([RELEASES.md](RELEASES.md)).

## Wat de eigenaar ziet

**Systeem → Firmware** toont:
- de geïnstalleerde versie (`v1.2.3` of *Ontwikkelversie*) en de volledige commit;
- het actieve geheugenslot (`app0`/`app1`) en zijn status (*bevestigd*, *proefstart*);
- het andere slot en zijn status (*bevestigd* = terugvaloptie, *ongeldig* of *afgebroken* = een mislukte of teruggedraaide update, *leeg of onbekend*);
- wanneer er voor het laatst is gecontroleerd en de nieuwste officiële versie, met korte releasenotes;
- de knoppen **Controleer op updates** en, alleen bij een nieuwere versie, **Update installeren**.

Na **Update installeren** vraagt de pagina eerst om bevestiging. Daarna volgt ze de update stap voor stap:

1. downloaden en controleren, met voortgang;
2. herstarten;
3. proefstart;
4. **Update geslaagd** — pas zodra de nieuwe firmware zichzelf als geldig heeft bevestigd (`running_state: valid`).

Wordt de nieuwe firmware niet bevestigd, dan meldt de pagina dat de controller automatisch is teruggekeerd naar de vorige versie. Tijdens de download en de herstart weigert de controller RF-bediening, Siri, koppelen, uitlezen en herstarten (`update-busy`).

De handmatige `.orconfw`-upload blijft bestaan, onder *Handmatig installeren*, en wordt nu op dezelfde manier gevolgd.

De pagina volgt een installatie vanaf de klik via `/api/ota`, dat geen login nodig heeft. Ook een gepauzeerd tabblad dat de fase *geschreven* mist, of een sessie die door de herstart verdween, eindigt dus bij de echte uitkomst en niet op het inlogscherm.

Na het schrijven blijft de schrijver dicht tot de herstart (`restart-pending`). Een handmatige upload herstart ook als de browser de verbinding na de laatste byte verbreekt.

**Beide installatieroutes**, de éénknopsupdate en de handmatige upload, blokkeren tot de herstart:
- RF-bediening, Siri, koppelen en uitlezen;
- herstarten en WiFi opslaan.

Een installatie start niet terwijl er al een herstart klaarstaat, of terwijl een sessie of RF-commando loopt.

Een afgewezen of afgebroken image wordt onbootbaar gemaakt (eerste sector gewist), zodat de noodscan van de bootloader het nooit start. Restrisico: een stroomstoring tussen de laatste byte en de controles.

Een proefstart die zichzelf niet kan bevestigen, valt na 300 s ook terug.

Ook op images zonder updatecontrole (RX-only, RF-test) blokkeert een handmatige upload RF en sessies tot de herstart.

Een geweigerde upload wordt meteen gesloten, nog vóór de inhoud: de webserver wacht onbeperkt op bytes. Restrisico: een eigenaar die zelf een upload halverwege laat hangen, houdt de webloop vast tot de verbinding sluit.

Er wordt niet geschreven naar het slot dat de bootloader nog als opstartkeuze heeft (`boot-slot-mismatch`; alleen na een terugval van de bootloader).

Een onbevestigde update die de eigendomsopslag niet kan lezen, keert na 300 s alleen terug als het andere slot een bevestigd image van het eigenaarsmodel is. Anders geldt OWNER-10: RST.

**Lus-watchdog (Codex r5).** De webserver van Arduino 2.0.16 kan op een misvormd multipart-verzoek eindeloos blijven lussen, nog vóór enige routecode draait. Een taak op core 0 herstart daarom het bord als de hoofdlus niet meer tikt:
- na 20 s bij een proefstart, een geselecteerd image of een geplande herstart (een herstart in een proefstart betekent rollback);
- na 120 s in alle andere gevallen.

Verder:
- Het PRG-menu en WiFi-opslaan wachten op een installatie, op alle builds.
- "Teruggedraaid" meldt de pagina alleen na een geziene proefstart.
- Een verloren antwoord op het installatie- of uploadverzoek wordt via `/api/ota` toch gevolgd.

Herstartplanning is wrap-veilig. De status toont de fout van de schrijver nooit terwijl de download die bezit. Gelukt betekent: commit, releasenummer, het andere slot én *valid*.

## Hoe het werkt

| Stap | Wat | Waar |
|---|---|---|
| Controle | `GET https://github.com/MarcoHuib/orcon-15rf-controller/releases/latest/download/orcon-update.json` → doorverwijzing naar `releases/download/vX.Y.Z/…` → doorverwijzing naar `release-assets.githubusercontent.com` | `fw_update.cpp` `check()` |
| Index | `{"schema":1,"manifest":"<getekend v2-manifest>","sig":"<DER hex>","notes":"…"}`, maximaal 2048 B | `parseIndex()` |
| Oordeel | Handtekening met de releasesleutel (`ota_pubkey.h`), dan `parseManifest` en: v2, variant `control`, env `heltec_v2_sx1276_control`, past in het slot, **nieuwer** dan de eigen `ORCON_RELEASE_VERSION` | `judge()` |
| Installatie | Alleen na `POST /api/update/install {"version":N,"commit":"…"}` met exact de aangeboden build (versie én commit): `GET …/releases/download/vX.Y.Z/orcon-control-vX.Y.Z-<commit7>.orconfw`. De URL wordt **op het apparaat** gebouwd uit het getekende manifest; de browser levert nooit een URL of bestand. | `install()` |
| Bundel | Eerste regel exact `{"manifest","sig"}` en byte-gelijk aan wat de controle verifieerde; daarna precies `size` bytes. Content-Length moet precies kloppen. | `install()` |
| Schrijven | Dezelfde controles en schrijfroute als de handmatige upload (`acceptManifest` → `startWrite` → `writeChunk` → `endWrite`): handtekening, manifest, versie, slotgrootte, SHA-256, variant-markers, commit-stempel, ESP-IDF-imagecontrole, dan pas het opstartslot. | `ota_signed.cpp` |
| Herstart | 3 s na het schrijven, in een proefstart met rollback (bestaande `ota::tick`: bevestigd na 60 s met WiFi en RX, anders terug na 300 s). | `fw_update_esp.cpp`, `ota_signed.cpp` |

**Automatisch controleren.** De eerste controle volgt 3 minuten na het opstarten, maar niet tijdens een proefstart. Daarna eens per 24 uur, en na een mislukte controle eens per uur. Handmatig controleren kan hooguit eens per minuut (`too-soon`).

Een controle installeert nooit iets. Installeren vraagt altijd:
- de aangemelde sessie van de eigenaar;
- het CSRF-token;
- dezelfde origin;
- het thuisnetwerk, dus niet het setup-AP.

## Beveiliging

- **HTTPS met eigen trust-anker.** Alleen de 9 root-CA's in `src/fw_update_roots.h` worden vertrouwd:
  - ISRG X1/X2 en Root YR/YE;
  - USERTrust ECC/RSA;
  - Sectigo E46/R46;
  - DigiCert G2.

  Op 2026-11-19 verifiëren github.com, release-assets.githubusercontent.com en objects.githubusercontent.com tegen precies deze set. Root YR/YE zijn geverifieerd via dezelfde publieke sleutel in hun kruisondertekende versies. `WiFiClientSecure` krijgt deze set als CA-buffer, dus mbedTLS draait met `VERIFY_REQUIRED` en controleert de hostnaam (SNI).
  - Er is **geen `setInsecure()`**. `tools/test_tx_poc_safety.py` en de workflowguard zoeken daarop.
  - De vingerafdrukken staan in het headerbestand. Een wijziging is een review-onderwerp.
- **Alleen toegestane adressen.**
  - `github.com`, alleen onder `/MarcoHuib/orcon-15rf-controller/releases/`, zonder `..` of `%`.
  - `release-assets.githubusercontent.com` en `objects.githubusercontent.com`.
  - Maximaal 3 doorverwijzingen, alleen `https://` of een absoluut pad op dezelfde host.
  - Andere hosts, `http://`, `//host`, user-info en andere poorten dan 443 worden geweigerd, nog vóór er verbinding wordt gemaakt.
- **Limieten.**

  | Wat | Limiet |
  |---|---|
  | Headerregel | 1648 B |
  | Location | 1615 tekens (een echte asset-URL is ~930) |
  | Responskop | 16 KB, 64 regels |
  | Index | 2048 B |
  | Notes | 400 B UTF-8, zonder stuurtekens |
  | Bundelkop | 1024 B |
  | Content-Length | verplicht, exact |

  - Chunked en Content-Encoding worden geweigerd, net als dubbele of tegenstrijdige Content-Length en dubbele Location.
  - Timeouts: verbinding/handshake 15 s, stilstand 15 s, de hele controle 45 s, de hele download 10 min. Elke stap krijgt hooguit de resterende tijd, en na de deadline wordt niets meer geselecteerd.
  - Strikt:
    - veldnamen zijn HTTP-tokens;
    - de index en de bundelkop zijn precies één JSON-object, zonder dubbele sleutels, escapes in sleutels of naloop;
    - notes zijn geldige UTF-8 (geen overlong, geen surrogates).
- **Geheugen.** Alle buffers hebben een vaste grootte, zonder groeiende allocaties.
  - Per taak wordt één context van ~10 KB op de heap gezet en weer vrijgegeven: URL, responskop, I/O-buffer van 4 KB en de aanbieding.
  - De workertaak heeft een stack van 12 KB en wordt na de taak verwijderd.
  - Een taak start alleen bij ≥ 90 000 B vrije heap en een vrij blok van ≥ 32 000 B. TLS heeft twee recordbuffers van 16 KB nodig, plus de roots en de handshake. Anders volgt `low-memory`.
- **Geen blokkade van RF of web.** De worker draait op core 0 met prioriteit 1, onder WiFi en lwIP. De RX-taak en de Arduino-loop draaien op core 1.
  - De worker geeft minstens elke 100 ms één tick af, zodat de idle-taak van core 0 nooit de task-watchdog (5 s, panic) raakt, ook niet bij een snelle download.
  - De worker raakt de RF-console en de webserver niet aan.
  - `tick()` in de loop logt en herstart.
  - Tijdens het flashschrijven kan RF-ontvangst kort haperen, net als bij de handmatige upload: flash-erase pauzeert de cache.
- **Geen credentials.** Er zit geen token in de firmware, en alleen publieke URL's worden gebruikt (geen API-ratelimit van 60 per uur).
  - De User-Agent is `orcon-heltec-v2`.
  - Privacy: GitHub ziet het IP-adres van je netwerk bij elke controle (dagelijks).

## Bekende beperkingen (eerlijk)

1. **Certificaatdatums worden niet gecontroleerd.** De mbedTLS van Arduino-ESP32 2.0.16 is gebouwd zonder `MBEDTLS_HAVE_TIME_DATE`. Een verlopen maar verder geldig certificaat van een van de 9 CA's wordt dus geaccepteerd.
   - De **firmware-handtekening is daarvan onafhankelijk**. Een aanvaller met zo'n certificaat kan geen firmware installeren; hij kan hooguit een update tegenhouden of een oude, echte release opnieuw aanbieden. Die wordt dan als oud geweigerd.
2. **CA-wissel door GitHub.** Kiest GitHub een CA buiten deze 9, dan faalt de controle met `tls-failed`. Herstel: een handmatige `.orconfw`-upload met een firmware die de nieuwe root kent.
3. **Dynamisch geheugen niet gemeten op hardware.** `/api/update` meldt na elke taak:
   - `mem.heap_min_job` (laagste vrije heap tijdens de taak, ook gemeten tijdens de TLS-sessie);
   - `mem.block_min_job` (kleinste grootste vrije blok);
   - `mem.stack_free_job` (ongebruikte stack).

   Meet die bij de hardwarevalidatie.
4. **Geen RTC.** *Laatst gecontroleerd* is relatief ("2 uur geleden"), gemeten vanaf de start van de controller.
5. **De RX-only-image heeft geen updatecontrole.** Alleen de control-image wordt gepubliceerd. De RX-only-image houdt de handmatige upload.

## Gemeten op de build (zelfde stempel, `pio run`, 2026-11-19)

| control-image | main 29bf74a | deze branch | verschil |
|---|---|---|---|
| firmware.bin | 1.085.376 B | 1.230.768 B | +145.392 B (slot 3.342.336 B: 37 % gebruikt) |
| statisch RAM (data+bss) | 86.268 B | 88.604 B | +2.336 B |
| IRAM `.iram0.text` | 78.827 B | 78.899 B | +72 B |

Het grootste deel is de TLS-client: mbedTLS SSL/X.509 en WiFiClientSecure. Bootloader en partitietabel blijven byte-identiek aan de gepinde bestanden.

## Fouten (in de pagina in het Nederlands)

`no-network`, `dns-failed`, `tls-failed`, `connect-failed`, `timeout`, `connection-lost`, `bad-response`, `redirect-not-allowed`, `too-many-redirects`, `rate-limited` (403/429), `server-error`, `http-error`, `unsupported-encoding`, `response-too-large`, `no-release`, `bad-index`, `bad-signature`, `wrong-variant`, `older-release`, `asset-missing`, `bundle-mismatch`, `bad-bundle`, `size-mismatch`, `low-memory`, `too-soon`, `no-update-offered`, `update-busy`, `confirm-running-image-first`. Daarnaast de bestaande OTA-codes, zoals `hash-mismatch`, `image-not-control` en `flash-write-failed`.

## Tests

| Wat | Soort | Waar |
|---|---|---|
| SemVer, URL's, allowlist, kop-limieten, mock-GitHub (redirects, DNS/TLS, 403/429/5xx, stilstand, afbreken, chunked, te groot), index (handtekening, manipulatie, variant, downgrade, slotgrootte), download (afgebroken, OTA-schrijffout, gemanipuleerd image, andere bundel, naloopbytes, vreemde redirect), geheugengrens | host, 169 checks | `test/test_fw_update.cpp` |
| Routes: eerst de eigenaarssessie, gepinde TLS-roots, alleen de aangeboden versie (ook als een controle tegelijk afrondt), schrijver dicht na selectie | bronguard + mutaties | `tools/test_tx_poc_safety.py` |
| Webinterface: controle, bevestiging, alleen het getoonde versienummer, voortgang, herstart, proefstart, bevestigd, rollback, fouten | host (node) | `test/test_setup_page.js` |
| Geen RF, Siri, koppelen, uitlezen of herstart tijdens een update | host | `test/test_setup_web.cpp` (control) |
| Firmware compileert, geen verboden OTA-strings, boot-bestanden gepind | build | `pio run -e heltec_v2_sx1276_control` |
| TLS-handshake, heap en stack, echte download, proefstart, rollback | **hardware: NOT TESTED** | [RELEASES.md](RELEASES.md) checklist |
