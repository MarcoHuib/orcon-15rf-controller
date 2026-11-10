# Installatiewizard en setup-API

`/` is de installatiewizard: koppelen, een fysieke afstandsbediening uitlezen en dagelijks
bedienen. De bestaande bedieningspagina staat op `/dashboard`. Alle andere pagina's en API's
zijn ongewijzigd; hun "terug"-links wijzen nu naar `/dashboard`.

| Bestand | Rol |
|---|---|
| `src/setup_page.html` | Bron van de pagina (HTML + CSS + vanilla JS, systeemfonts, geen afbeeldingen) |
| `src/setup_page.h` | Gegenereerd: gzip -9 als PROGMEM-array (`python3 tools/gen_setup_page.py`) |
| `src/setup_flow.{h,cpp}` | Sessielogica zonder Arduino-afhankelijkheden, host-getest |
| `src/setup_web.{h,cpp}` | HTTP-routes op de bestaande `WebServer` en ArduinoJson |
| `src/main.cpp` | Elk gedecodeerd frame uit de bestaande RX-drain gaat ook naar `setupweb::onFrame` |

Niet in de TX-PoC dry-run: `setup_web.cpp` staat volledig onder `#if !ORCON_TX_POC`. De
symbooltabel van de PoC-ELF is identiek aan die van de basis (`33a6914`).

## Capabilities (uit de build, niet uit de pagina)

| Build | `rx_supported` | `tx_supported` | `read_supported` | `pair_supported` | `control_supported` |
|---|---|---|---|---|---|
| `heltec_v2_sx1276` (RX_ONLY) | ja | nee | ja | nee | nee |
| `heltec_v3_cc1101_ota`, `esp32-480480s040` | ja | ja | ja | ja | ja |
| `heltec_v3_1262_ota` (SX1262 decodeert geen RAMSES) | nee | ja | nee | nee | ja |
| `heltec_v2_sx1276_tx_poc_dryrun` | Wizard en API niet aanwezig | | | | |

In RX_ONLY-builds is er geen code die `startPairing`, `setMode` of `setTimer` aanroept;
`/api/remote/*` antwoordt `501 tx-unavailable`. De ELF-check (`tools/check_elf_tx.py`)
blijft OK en `radio::sendFrame` wordt niet gelinkt.

## Beveiliging van POST

Elke POST moet aan alle voorwaarden voldoen, anders `403`/`413`/`400`:

1. `Host` is het eigen IP van het apparaat (`net::ipAddr()`, met optionele poort) of een
   `.local`-naam. Dat beschermt tegen DNS-rebinding. Onder een andere naam geeft
   `GET /api/setup/state` ook geen token.
2. `Content-Type: application/json`. Een cross-site formulier of simple request kan dit niet
   zonder CORS-preflight sturen, en die preflight wordt niet beantwoord.
3. Als er een `Origin` is, is die exact `http://<Host>`.
4. `X-Orcon-Token` is gelijk aan het token per boot (16 random bytes, vergelijking in
   constante tijd). Het token staat in `GET /api/setup/state`.
5. De body is maximaal 160 B en een JSON-object met alleen bekende velden.

**Grens:** dit is bescherming tegen ongeautoriseerde browserverzoeken (CSRF, rebinding),
geen gebruikersauthenticatie. Een LAN-client die zelf `GET /api/setup/state` doet, kan het
token lezen. Dat geldt ook voor de bestaande, onbeschermde `/api/cmd`, `/api/ids` en
`/api/wifi`. Echte authenticatie vraagt een geheim buiten HTTP (zoals de OLED-sleutel van de
PoC) en is hier niet gebouwd.

## Contracten

Antwoorden zijn compacte JSON, opgebouwd met `snprintf` in één vaste buffer van 1536 B (geen
heap-`String`), met `Cache-Control: no-store`. Fouten zien er altijd zo uit:
`{"error":"<code>"}`. GET wijzigt niets aan de wizardsessies; time-outs lopen in
`setupweb::tick()` in de main loop.

### `GET /` (pagina)
`200`, `Content-Encoding: gzip`, `text/html; charset=utf-8`, CSP, `X-Frame-Options: DENY`.
Zonder `gzip` in `Accept-Encoding` volgt `406`: de pagina staat alleen gecomprimeerd in flash.

### `GET /api/setup/state`
```json
{"fw":"…","rx_only":true,"caps":{"rx_supported":true,"tx_supported":false,"read_supported":true,
 "pair_supported":false,"control_supported":false},"rf":"receiving|recovering|failed|unsupported",
 "token":"<32 hex | leeg>","mask_ids":true,"active":"none|read|pair",
 "unit":{"known":true,"id":"29:000202","mode":"low","age_s":12},
 "remotes":[{"id":"29:000101","age_s":3,"rssi":-61,"cmd":"22F1","n":4}]}
```
`remotes` is alleen discovery: apparaten die in de laatste 30 min een `22F1`/`22F3` stuurden
(uit `rlog`). Er wordt nooit iets automatisch gekozen. De pagina maskeert ID's als
`mask_ids` aan staat.

### Uitlezen
| Endpoint | Body | Succes | Fouten |
|---|---|---|---|
| `POST /api/setup/read/start` | `{"remote":"29:000101"}` | `201` + read-state | `400 bad-remote`, `404 remote-not-heard`, `409 session-busy`, `501 read-unsupported`, `503 rf-unavailable` |
| `GET /api/setup/read/state` | – | `200` | – |
| `POST /api/setup/read/cancel` | `{"sid":3}` | `200` + read-state | `409 no-such-session` |

Read-state:
```json
{"sid":3,"status":"idle|active|done|timeout|cancelled","step":2,"remote":"29:000101","left_s":48,"ev":9,
 "rf":"receiving","others":1,"why":"accepted|duplicate|wrong-button|unknown-command|before-step|",
 "why_ev":8,"why_cmd":"22F1","why_mode":3,
 "steps":[{"key":"away","ok":true,"cmd":"22F1","pl":"000004","rssi":-61,"ev":5},{"key":"timer","ok":false},…]}
```

### Koppelen (alleen TX-builds)
| Endpoint | Body | Succes | Fouten |
|---|---|---|---|
| `POST /api/setup/pair/start` | `{}` | `201`, status `ready`. Er wordt nog niets verzonden | `409 session-busy`, `501 tx-unavailable` |
| `POST /api/setup/pair/press` | `{"sid":4,"keys":["auto","1"]}` | `200`, status `bind_sent` | `400 bad-keys`, `409 no-such-session`/`already-sent`, `422 unsupported-combination`, `502 tx-failed` |
| `GET /api/setup/pair/state` | – | `{"supported","sid","status","bind_sent","rf_confirmed","paired","left_s"}` | – |
| `POST /api/setup/pair/cancel` | `{"sid":4}` | `200` | `409 no-such-session` |

De statussen zijn: `idle`, `ready`, `bind_sent`, `tx_failed`, `rf_confirmed`, `paired`,
`timeout` en `cancelled`. Er zijn vier aparte stappen:

1. **Bedieningscommando:** zie hieronder.
2. **Bind-request:** de combinatie Auto + 1 komt als één verzoek binnen en roept één keer de
   bestaande `orcon::startPairing()` aan (een `1FC9`-aanbod plus een venster van 3 min).
   Twee losse drukken tellen nooit op, en een bind-request wordt nooit herhaald.
3. **RF-bevestiging:** een `1FC9` (geen RQ) van een ander apparaat, geadresseerd aan ons ID.
4. **Gekoppeld:** de RF-bevestiging én de firmware (`orcon::pairState()==Paired`) heeft
   **hetzelfde** apparaat als unit vastgelegd. Alleen dat leidt tot `paired`.

Een verzonden aanbod kan niet worden ingetrokken. Na `cancel` loopt het venster van
`orcon` gewoon af (bestaand gedrag).

### Ventilatie bedienen (alleen TX-builds)
| Endpoint | Body | Succes | Fouten |
|---|---|---|---|
| `POST /api/remote/send` | `{"key":"away|auto|1|2|3"}` of `{"key":"timer","minutes":15|30|60}` | `202` + remote-state | `400 bad-key`/`bad-minutes`/`unknown-field`, `409 unit-not-selected`/`session-busy`, web-control: `409 profile-required`/`profile-protocol-unsupported`/`profile-unit-mismatch`, `429 too-fast`, `502 tx-failed`, `501 tx-unavailable` (RX_ONLY) |
| `GET /api/remote/state` | – | `{"supported","id","key","minutes","accepted","sent","confirm","age_s","unit_mode","unit_age_s"}` | `501` op RX_ONLY |

- **Vertaling, alleen server-side:** away/auto/1/2/3 gaan naar de bestaande
  `orcon::setMode()` (22F1 `00/04/01/02/03`). Timer gaat naar de bestaande
  `orcon::setTimer(min)` (22F3, hoog voor 15/30/60 min, zoals de 15RF-timerknop). Raw
  payloads worden geweigerd (`unknown-field`).
- **Drie statussen:** `accepted` (gevalideerd), `sent` (`txCount` van de radio is gestegen) en
  `confirm`:
  - `pending`;
  - `confirmed`: de gekozen unit meldt die stand zelf in een `31D9`, binnen 10 s na verzending;
  - `not_confirmed`;
  - `not_available`: de timer heeft geen unitmelding die hem bewijst;
  - `none`: er is niets verzonden.
- **Geen flooding:** minimaal 1,5 s tussen commando's en nooit een automatische retry. De
  bestaande `setMode()` stuurt per commando twee `22F1`-kopieën met 120 ms ertussen. Dat
  is ongewijzigd gedrag en blokkeert de loop zo lang (net als `/api/cmd`).

## Uitleesregels (`setup_flow.cpp`)

- **Bron:** alleen frames uit de bestaande decoder en queue, in dezelfde volgorde als
  `orcon::onFrame`. Er is geen extra radiolus en de decoder is ongewijzigd.
- **Knop uit pakket:** alleen I-frames. `22F1` met modusbyte `00` (Woning), `04` (Auto) of
  `01/02/03` (Stand 1–3); de derde byte verschilt per model (`07` HRC400, `04` MVS-15RHBP)
  en wordt genegeerd. Elk `22F3` I-frame is Timer (minuten en staart verschillen).
  `05/06/07` (auto2, boost, uit), andere codes, RQ en RP tellen niet mee.
- **Eén afstandsbediening:** alleen frames waarvan de afzender (`addr0`) de expliciet gekozen
  afstandsbediening is. Andere apparaten worden alleen geteld (`others`); er worden geen ID's
  bewaard. Een `31D9` van de unit is nooit bewijs voor een knop.
- **Monotone event-ID:** `ev` loopt per boot op voor elk ontvangen frame.
- **Niet met terugwerkende kracht:** bij activatie van een stap wordt de sync-teller van de
  SX1276 (`coreSyncHits`) bewaard. Een frame met een sync-nummer dat niet groter is, begon
  vóór de activatie en krijgt `before-step`. Elke voltooide stap activeert de volgende met
  het sync-nummer van dat pakket. Radio's zonder sync-nummers krijgen een guard van 250 ms.
- **Dubbel:** een pakket dat identiek is aan het laatst geaccepteerde (afstandsbedieningen
  herhalen elke druk) wordt `duplicate` en genegeerd.
- **Time-out:** 60 s per stap zonder geldige druk. Bij RF-uitval (`rf` ≠ `receiving`) toont
  de pagina dat; er komt geen voortgang, alleen een time-out.
- **Opslag:** zes stappen van elk ≤ 8 payloadbytes in RAM. Er is geen geschiedenis en er wordt
  niets naar NVS geschreven.

## Pagina

- Drie schermen plus start:
  - koppelen;
  - uitlezen (kies eerst je afstandsbediening uit de gehoorde lijst, daarna zes stappen
    grijs → oranje → groen);
  - bedienen (permanent bereikbaar).
- Navigatie via `#pair`, `#read` en `#control`, dus de terugknop van de browser werkt.
- Polling: 1 s tijdens uitlezen en bij een openstaande bevestiging, 2 s tijdens koppelen,
  anders 2,5–10 s. De polling pauzeert als het tabblad verborgen is.
- Groen en "Gekoppeld" verschijnen alleen op basis van een serverantwoord. Een oude,
  afgeronde sessie wordt niet als het huidige resultaat getoond.
- Knoppen zonder capability zijn uitgeschakeld en de handlers controleren de capability zelf
  ook.
- Een fout van een actie blijft staan tot de volgende actie; een verbindingsfout verdwijnt
  bij de volgende geslaagde poll.

## Gemeten groottes (lokaal, 2026-11-10)

| | Basis `33a6914` | Nieuw | Verschil |
|---|---|---|---|
| Pagina | – | 23 022 B HTML, 8 568 B gzip in flash | Richtwaarde was ≤ 20 KB / < 8 KB |
| `heltec_v2_sx1276` firmware.bin | 1 043 584 B | 1 063 632 B | +20 048 B |
| `heltec_v2_sx1276` statisch RAM | 68 408 B | 70 232 B | +1 824 B (responsebuffer 1 536 B) |
| `heltec_v3_cc1101_ota` firmware.bin | 1 412 272 B | 1 448 080 B | +35 808 B |
| `heltec_v3_cc1101_ota` statisch RAM | 70 656 B | 72 464 B | +1 808 B |
| `heltec_v2_sx1276_tx_poc_dryrun` | 897 632 B | 897 616 B | Identieke symbooltabel; alleen pad- en tijdstempels verschillen |

De pagina is 2,8 KB boven de richtwaarde (gzip 0,6 KB). Dat komt door de echte
statusafhandeling en foutteksten, die de demo niet had.

## Tests (lokaal, geen GitHub Actions)

```sh
test/run_host_tests.sh                   # alle suites, inclusief:
#   setup         97 checks  sessielogica met synthetische RF-frames
#   setup_web_rx  47 checks  echte setup_web.cpp + orcon.cpp, RX_ONLY=1, nep-WebServer en -radio
#   setup_web_tx  76 checks  idem, RX_ONLY=0 (koppelen, bedienen, TX-fouten)
#   gen_setup_page.py --check, node test/test_setup_page.js (30 checks, de echte paginascript)
CXXFLAGS="-fsanitize=undefined -fno-sanitize-recover=all" test/run_host_tests.sh
pio run -e heltec_v2_sx1276              # en de andere envs
```

`setup_web_*` heeft de ArduinoJson-versie nodig die `pio run -e heltec_v2_sx1276` ophaalt.
Zonder die versie meldt de runner `NOT TESTED`.

**NOT TESTED:**
- Alles op echte hardware: er is niets geflasht.
- De echte `WebServer`-afhandeling van headers en gzip.
- Payloads van de timerknop van de MVS-15RHBP-afstandsbediening.
- Het `1FC9`-bevestigingsformaat van een echte unit; volgens `PROTOCOL.md` §5 is binding
  best-effort.
- Bediening en koppelen op een TX-build met radio.
- ASan/TSan (ASan hangt op deze Mac vóór `main`).
