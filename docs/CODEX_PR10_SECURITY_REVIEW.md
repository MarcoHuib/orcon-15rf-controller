# Onafhankelijke beveiligingsreview van PR #10 (Codex) — 2026-11-14

**Wat is beoordeeld:** PR #10 (`feature/production-control`) ten opzichte van `main` (6807159).
- Ronde 1: 2de52dd.
- Ronde 2: e45d0ba, de herstelcommit.

**Uitvoering:**
- Codex CLI 0.169 (VS Code-extensie), in een losse kloon met alleen de publieke branchgeschiedenis.
- Niet in de kloon: build-uitvoer, captures, `.flashtools` en lokale refs.
- Sandbox: schrijven alleen in die kloon.
- Niet gebruikt: hardware, seriële poort, netwerk, GitHub, push en GitHub Actions.

## Ronde 1 (2de52dd): NO-GO

| ID | Ernst | Bevinding | Bewijs |
|----|-------|-----------|--------|
| SEC-01 | MAJOR | `POST /api/ota/hold` (rollbacktest) vroeg niet om aanmelden. Wie in het LAN zit, kon tijdens een proefstart elke update laten terugdraaien, ook een beveiligingsupdate. De route bestond al in `main`. | probe: anoniem cross-origin-verzoek gaf 200; na 300 s volgde de rollback |
| SEC-02 | MINOR | Na de wrap van `millis()` (2^32 ms, 49,7 dagen) werden verlopen sessies, herstelcodes en nonces weer geldig. | probe: op uitgifte + 2^32 + 1 ms werden sessie, nonce en code geaccepteerd |
| SEC-03 | MINOR | Een verlopen aanmeldblokkade kwam na ~24,8 dagen terug als blokkade van weken, zelfs voor het juiste wachtwoord. | probe: wachttijd 2 048 597 296 ms op dag 26 |
| SEC-04 | INFO | Platte HTTP: wie het verkeer afluistert, ziet wachtwoord, herstelcode en sessiecookie. | alleen redenering (al gedocumenteerd in `docs/CONTROL.md`) |

## Herstel (e45d0ba)

**SEC-01: rollbacktest alleen voor de aangemelde eigenaar**
- `handleHold` begint met `setupweb::authorize(true)`. Dat vraagt sessie, CSRF-token en dezelfde origin.
- Op web-control-builds is de route nooit bereikbaar via het setup-accesspoint.
- Waar geen login bestaat (de PoC-dry-run), is de route weggelaten.
- `orcon.py ota-install --test-rollback` vraagt het webwachtwoord vooraf en meldt zich in de proefstart zelf aan. Sessies staan in RAM en verdwijnen bij de herstart.

**SEC-02 en SEC-03: verlopen materiaal blijft weg**
- `Sessions/Throttle/Recovery::sweep()` wissen verlopen materiaal. `setupweb::tick` roept ze bij elke loopdoorgang aan, dus ruim vóór een wrap.
- De blokkade is nu een expliciete vlag. `waitMs` komt nooit boven de 15 minuten.
- De OTA-nonce krijgt dezelfde sweep in `ota::tick`.

**Tests**
- `test_web_auth` krijgt de wrapprobes. Tegen het oude gedrag (sweeps uitgeschakeld) geven ze 6 fouten.
- De CLI-test controleert de volgorde: eerst aanmelden, dan de hold met cookie, token en origin.
- Een bronbewaking controleert dat hold eerst autoriseert, met een mutatietest.

## Ronde 2 (e45d0ba): GO

| ID | Resultaat |
|----|-----------|
| SEC-01 | FIXED. Geweigerd: anoniem, zonder of met verkeerd CSRF-token, cross-origin, vreemde Host, en via het setup-AP op web-control. Getest op RX-only, web-control dry-run en RF, RF-test en control. De eigenaar kan wel tegenhouden. In PoC-builds zijn handler en registratie weg. |
| SEC-02 | FIXED. Via de echte `setupweb::tick` verlopen sessies, nonces en codes definitief vóór een wrap. De OTA-nonce wordt ook gewist. |
| SEC-03 | FIXED. Wachttijd 0 op dag 26, en geen terugkeer over de wrap. |
| SEC-04 | NOT FIXED. HTTP blijft. De beperking is gedocumenteerd en wordt aangepakt in het provisioningontwerp. |

**Uitslag:** "VERDICT: GO for merge (security)". Daarnaast leverde de diff `main...e45d0ba` geen nieuwe bevindingen op.

**Niet getest (Codex):**
- volledige firmwarebuilds;
- de ESP32-headerverwerking;
- mbedTLS en NVS op het apparaat;
- hardwaregedrag.

**Lokaal na de herstelcommit:**
- de 23 C++-hostsuites, 38 CLI-checks en 121 paginachecks: PASS;
- `heltec_v2_sx1276`, `_control` en `_tx_poc_dryrun` bouwen.
