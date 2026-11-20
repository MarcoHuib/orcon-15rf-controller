# Weg met de Legacy-toestand: één herstelsleutel per Heltec

> Branch `feature/remove-legacy-recovery`, bovenop `feature/github-releases-control-ota` (PR #16).
> - Er is niets geflasht, gereset of aan eigenaarsgegevens gewijzigd.
> - Elke stap op het board gebeurt alleen met jouw akkoord.

## Doelarchitectuur

| | Wie heeft hem | Waarvoor | Nooit voor |
|---|---|---|---|
| Firmware-signingsleutel | alleen GitHub Actions (Environment `orcon-production-release`) | "deze firmware is officieel" | een wachtwoord zetten of herstellen, in welke toestand ook |
| Eigenaars-/herstelsleutel | de eigenaar, per Heltec (`orcon-recovery-key.pem`, versleuteld) | wachtwoordherstel, alleen in de fysieke herstelmodus (PRG-menu) | firmware |
| Beheerderswachtwoord | de eigenaar | dagelijks aanmelden, updates goedkeuren | — |

**Bij een normale OTA-update blijven behouden:**
- NVS: WiFi, profielen, RF-identiteit, wachtwoordhash, eigenaarssleutel en Siri-sleutel.

**Nieuwe apparaten:**
- Ze worden alleen via de installatiewizard ingericht: setup-AP, stap 1 tot en met 4.
- Een eerste wachtwoord zetten kan alleen in de toestand New.

## Wat er tot nu toe was (de Legacy-paden)

| Onderdeel | Waar (PR #16 / main) | Wat het deed |
|---|---|---|
| `provision::State::Legacy`, `STORED_LEGACY = 3`, `migrate` | `provision.{h,cpp}`, `owner_store.cpp begin()` | Een wachtwoord zonder staat (firmware van vóór het eigendomsmodel) werd *legacy* en kreeg waarde 3 in NVS. |
| `vendorRecoveryOk()` | `setup_web.cpp` | Liet de drie routes hieronder toe zolang de staat *legacy* was. In builds zonder eigendomsmodel: altijd. |
| `POST /api/auth/nonce` | `setup_web.cpp handleAuthNonce` | Gaf een nonce en het MAC-adres, om te ondertekenen. |
| `POST /api/auth/reset` | `setup_web.cpp handleAuthReset` | Gaf een herstelcode van 10 minuten voor een verzoek dat met de **firmware-signingsleutel** was ondertekend (`ota::verifySignature`). |
| `POST /api/auth/setup` | `setup_web.cpp handleSetup` | Zette met die code een nieuw wachtwoord, zonder in te loggen, vanaf het thuisnetwerk. |
| `webauth::Recovery`, `checkResetRequest`, `checkName`, `CODE_TTL_MS`, `CODE_TRIES`, `NONCE_TTL_MS` | `web_auth.{h,cpp}` | De code, de nonce en het verzoekformaat `orcon-auth-reset`. |
| `authstore::ownMac`, `authstore::verifySignature` | `auth_store.{h,cpp}` | De koppeling van de weblogin aan de OTA-sleutel. |
| `orcon.py auth-code`, `reset_request` | `tools/orcon.py` | De Mac-kant: ondertekenen met `~/.orcon/ota-signing-key.pem`. |
| Codeformulier en `auth-code`-uitleg | `setup_page.html` (`aHint`, `aCodeBox`, `aNew`, `aSetGo`) | "Wachtwoord vergeten" op een legacy-board. |
| Legacy-uitzondering voor Siri | `setup_web.cpp handleSiri` | Siri werkte in Done én Legacy. |

**Waarom het weg moet.** Op een board zonder herstelsleutel kan iemand met de signingsleutel plus toegang tot je netwerk het wachtwoord vervangen en daarna alles: bedienen, firmware installeren, WiFi wijzigen. Staat die sleutel in GitHub, dan wordt een GitHub-lek een lek van je controller.

## Wat deze wijziging doet

**Verwijderd:** alle onderdelen uit de tabel hierboven. Ook de routes bestaan niet meer: ze geven 404, in elke toestand en op elke build.

**Een board zonder herstelsleutel**, met waarde 3 in NVS of een wachtwoord zonder staat, leest nu als *done zonder sleutel*:
- Aanmelden met het bestaande wachtwoord werkt gewoon.
- Een herstelsleutel registreren kan, alleen in de PRG-herstelmodus. De bestaande route `/api/provision/key` ondersteunde dit al.
- Wachtwoordherstel zonder sleutel: `/api/provision/recover` antwoordt `no-owner-key`. Een vergeten wachtwoord kan dan alleen met de PRG-fabrieksreset worden gewist; daarbij gaan alle instellingen verloren.
- De firmware **schrijft niets** in NVS (geen migratie, geen herschrijving van waarde 3). Een ouder image in het andere slot leest dus precies wat het gewend is.

**De proefstartpoort.** Een nieuwe firmware die via OTA binnenkomt, bevestigt zich alleen als:
- WiFi en RX gezond zijn, zoals al het geval was;
- én er een eigenaarssleutel is geregistreerd en leesbaar is (`ota_signed.cpp ownerKeyRegistered()`).

Zonder sleutel wordt de proefstart nooit bevestigd. Na 300 s keert de vorige firmware terug, met de herstelmogelijkheid die die firmware heeft. Het board kan dus nooit via een update in een toestand komen waarin het oude herstel weg is en het nieuwe nog niet bestaat. De console meldt: "geen herstelsleutel geregistreerd: deze firmware wordt niet bevestigd…".

Een installatie via USB heeft geen proefstart. Op een board zonder sleutel geldt dan "done zonder sleutel" (zie hierboven): je kunt aanmelden en een sleutel registreren.

**Tests:**
- `test_provision` (173): de oude waarden 0 + wachtwoord en 3 lezen als Done, zonder schrijven.
- `test_setup_web` (11 varianten): de drie routes geven 404; een volledige migratie: aanmelden, geen sleutel buiten de herstelmodus, `no-owner-key` vóór de sleutel, sleutel registreren, wachtwoord ongewijzigd, herstel met het sleutelbestand.
- `test_web_auth` (59) en `test_setup_page.js` (157): geen codeformulier en geen `auth-code`-tekst.
- `test_orcon_cli` (55): `auth-code` bestaat niet meer.
- Veiligheidsguard: geen van de oude namen of routes in `src/` en `orcon.py`, en de proefstartpoort staat in `tick()`. Er zijn 8 gerichte mutaties, die allemaal worden geweigerd.

## Migratievolgorde voor jouw Heltec

**Uitgangssituatie** (`installed.txt`, niet op het board gecontroleerd):
- app1: b75fad3 control (draait);
- app0: 2edb423 RX-only;
- toestand *legacy*.

Beide images hebben het eigendomsmodel. Uit de broncode van beide commits volgt dat zij het oude herstel uitzetten zodra er een eigenaarssleutel in NVS staat (`fromStored`: sleutel aanwezig → Done → `vendorRecoveryOk()` geeft 403).

**Daarom kan de migratie zonder nieuwe firmware, op wat er nu draait.** Er wordt niets verwijderd voordat de nieuwe route werkt.

| # | Wie | Stap | Bewijs dat hij geslaagd is | Als het misgaat |
|---|---|---|---|---|
| 0 | jij, alleen lezen | Meld je aan op `http://orcon15rf.local`. Systeem → Herstelsleutel zegt "Nog geen herstelsleutel". | Aanmelden lukt; je kent het wachtwoord. | Wachtwoord kwijt? Dan nu, met de huidige firmware, nog `auth-code` vanaf de main-checkout. Pas daarna verder. |
| 1 | jij, fysiek | **Herstelmodus:** druk kort op RST en houd binnen 20 s PRG 15 s vast. Kies Herstelmodus: kort drukken = volgende, 2 s vasthouden = kiezen. | Het OLED toont "Herstelmodus" en het netwerk Orcon15RF-Setup met het wachtwoord. | Niets aan de hand: na 15 minuten of een RST start het board normaal. |
| 2 | jij | Telefoon op Orcon15RF-Setup, open `http://192.168.4.1` en meld je aan. Systeem → Herstelsleutel: kies een wachtwoordzin (minstens 10 tekens) en sla `orcon-recovery-key.pem` op. Kies het opgeslagen bestand en vul de wachtwoordzin in: het board opent het bestand zelf en registreert pas dan de sleutel. | De melding "Nieuwe herstelsleutel geregistreerd (vingerafdruk …)". Systeem → Herstelsleutel: "Geregistreerd". | `key-write-failed` of `key-mismatch`: opnieuw proberen. Het oude herstel bestaat dan nog. |
| 3 | jij | Bewaar het sleutelbestand en de wachtwoordzin elk op twee plekken (bijvoorbeeld de wachtwoordmanager en een USB-stick), niet samen met het beheerderswachtwoord. | — | Zonder bestand en wachtwoordzin is alleen nog een fabrieksreset mogelijk. |
| 4 | jij, alleen lezen | Na een normale herstart: `curl -s -X POST -H 'Content-Type: application/json' -d '{}' http://orcon15rf.local/api/auth/nonce` | Antwoord `403 {"error":"owner-key-required"}`. Er wordt niets uitgegeven. | Krijg je een nonce? Dan is de registratie niet blijvend: terug naar stap 1. |
| 5 | jij (aanbevolen) | Bewijs eenmalig dat het nieuwe herstel werkt: herstelmodus → "Wachtwoord vergeten" → het sleutelbestand, de wachtwoordzin en een nieuw (of hetzelfde) wachtwoord. | Aanmelden met dat wachtwoord lukt. | Mislukt het herstel, dan heb je nog je bestaande wachtwoord. Controleer bestand en wachtwoordzin. |
| 6 | jij | Zet het GitHub-secret: `gh secret set ORCON_OTA_SIGNING_KEY --env orcon-production-release --repo MarcoHuib/orcon-15rf-controller < ~/.orcon/ota-signing-key.pem` | `gh secret list --env orcon-production-release` toont de naam. | — |
| 7 | jij | PR #16 en daarna deze PR reviewen en mergen. Daarna de eerste release via de workflow, en Systeem → Firmware → Update installeren. | Na ongeveer 60 s: "nieuwe firmware bevestigd". De poort laat hem door omdat er een sleutel is. | Zonder sleutel of bij een probleem: na 300 s automatisch terug naar b75fad3. |
| 8 | automatisch | Bij de volgende release schuift b75fad3 uit het andere slot. | Beide slots bevatten firmware zonder enig Legacy-pad. | — |

**Waarom deze volgorde veilig is:**
- Bij stap 0 en 1 verandert er niets.
- Het oude herstel verdwijnt pas in stap 2, en alleen doordat de nieuwe sleutel er is. Dat zit in de huidige firmware zelf.
- Het rollback-image 2edb423 volgt dezelfde regel, dus een terugval heropent niets.
- De signingsleutel gaat pas in stap 6 naar GitHub, als hij aantoonbaar (stap 4) geen wachtwoord meer kan herstellen.
- De nieuwe firmware van stap 7 kan je niet buitensluiten: zonder sleutel bevestigt hij zich niet.

**Restpunt.** Raakt NVS precies de namespace `orconown` kwijt terwijl `orconauth` blijft, dan leest een **oud** image (b75fad3, 2edb423, PR #16) het board weer als *legacy*. Dat kan alleen door ingrijpen via USB of door schade aan het flashgeheugen; geen enkele route doet het. Na stap 8 staat er geen oud image meer op het board.

## Wat daarna definitief kan verdwijnen

**Weg in deze wijziging** (code, tests en docs): alles uit de tabel "Wat er tot nu toe was". Ook de `#error`-reden in `config.h` (wachtwoordherstel getekend met de OTA-sleutel) is aangepast.

**Kan daarna weg** (elk een eigen, kleine beslissing; niet in deze PR):

1. **De signingsleutel op de Mac** (`~/.orcon/ota-signing-key.pem` en de back-ups). Doelpunt 1 zegt: alleen GitHub. Na stap 6 en een geslaagde eerste release:
   - Bewaar één versleutelde kopie offline als noodsleutel.
   - Verwijder de Mac-kopie, alleen met jouw akkoord: ik verwijder geen sleutels.
2. **Board-gebonden v1-manifesten** (`/api/ota/nonce` en `/api/ota/begin` met `"v":1`, `ota_policy` v1, `orcon.py ota-install` en de v1-route in `heltec_v2.sh`). Die bestaan alleen om met de sleutel op de Mac lokaal te tekenen. Zonder Mac-sleutel blijven alleen v2-releases uit GitHub. Let op: daarmee verdwijnt ook de lokale ontwikkel-OTA.
3. **`orcon.py release`**: lokaal releasebundels tekenen. Dat wordt vervangen door de workflow.
4. **De oude TX-PoC-portals** (`heltec_v2_sx1276_tx_poc_dryrun`, `heltec_v2_sx1276_tx_poc`). Daar staat de weblogin nominaal aan zonder eigendomsmodel, met een sleutel per boot (`g_pocSecurity`). Ze worden niet als release gebouwd en de OTA-policy weigert ze (`image-predates-owner-model`). Ze zijn wel nog in gebruik in de lokale testsuite (`elf-selftest`).
5. **De OTA zonder authenticatie in `net.cpp`** (`#if !RX_ONLY`: ArduinoOTA en `/update`). Geen enkele Heltec-build heeft die nog (alle envs `RX_ONLY=1`). Alleen een host-testvariant compileert hem. Het apparaat weigert images waarin hij staat (`image-has-unauthenticated-ota`).
6. **`retirePreOwnerSlot`** in de fabrieksreset: die vervangt een image van vóór het eigendomsmodel. Na stap 8 is die stap voor jouw board niet meer nodig; voor een via USB geflasht ouder board blijft hij nuttig. Mijn advies: laten staan.
7. **Documentatie die `auth-code` noemt.** Bijgewerkt in deze PR: `CONTROL.md`, `PROVISIONING.md`, `RELEASES.md`, `RELEASESLEUTEL.md`, `MIGRATIE_EIGEN_HELTEC.md` en `HW_VALIDATIE_PROVISIONING.md`. Historische reviewrapporten (`CODEX_*`) blijven ongewijzigd.

## Hardwarevalidatie (na akkoord, per stap)

| # | Test | Verwacht |
|---|---|---|
| H1 | Stap 0 tot en met 5 hierboven, op b75fad3 | Zoals in de tabel. |
| H2 | Release met deze wijziging installeren op het gemigreerde board | Bevestigd na ongeveer 60 s; NVS ongewijzigd: WiFi, profiel, Siri en het wachtwoord werken. |
| H3 | `curl … /api/auth/nonce` op de nieuwe firmware | 404 |
| H4 | Herstel met het sleutelbestand op de nieuwe firmware (herstelmodus) | Nieuw wachtwoord werkt. |
| H5 | (Optioneel, alleen op een testboard zonder sleutel) dezelfde release installeren | Niet bevestigd; na 300 s terug naar de vorige firmware, met de consolemelding. |
