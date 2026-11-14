# Plan: eerste hardwarevalidatie van installatie, herstel en fabrieksreset

**Regels die altijd gelden**
- Geen flash, OTA, reset of RF zonder expliciete toestemming per stap.
- Geen GitHub Actions.
- Bewijs komt lokaal in `build/` en `captures/` (git-ignored).
- Wat niet getest is, heet **NOT TESTED**.

**Voorwaarde: een tweede Heltec V2 (868 MHz) voor de destructieve tests.** De werkende installatie (app1 a5260ad control, profiel, koppeling) raken we alleen aan in fase 3, met de migratiestappen. Is er geen tweede board, dan vallen fase 1 en 2 af. Fase 3 kan dan wel, behalve de fabrieksreset (die wist de koppeling).

## Fase 0 — zonder hardware

1. Host-tests: `test/run_host_tests.sh` (alle suites), `tools/test_orcon_cli.py`, `tools/test_tx_poc_safety.py` en `tools/check_privacy.py`.
2. Bouwen: alle omgevingen met `pio run`. Daarnaast het bewijspakket in een losse release-worktree (`LOCAL_USB_RELEASE.md`) met `heltec_v2.sh package` en `verify`, voor het RX-only- en het control-image.
3. Exit: alles PASS. Leg commit en hashes vast.

## Fase 1 — tweede board: eerste installatie (toestemming: USB-flash van het factory-image)

**Voorbereiding.** Lees het board eerst uit (backup van de flash). Flash daarna het factory-image (bootloader, partities, otadata, app0).

| # | Test | Verwacht | Bewijs |
|---|---|---|---|
| 1.1 | Eerste start | OLED: "Installatie", `Orcon15RF-Setup` en een wachtwoord van 12 tekens. Alleen AP, geen STA. Serial: `[owner] state new` | foto OLED, serieel log |
| 1.2 | Captive portal | iOS Safari en Android Chrome openen de wizard vanzelf. Handmatig `http://192.168.4.1` werkt ook | schermafbeeldingen |
| 1.3 | Stap 1 | Een fout (twee verschillende wachtwoorden) wordt geweigerd. Een goed wachtwoord geeft een sessie. Een tweede "eerste wachtwoord" krijgt 403 | HAR/console |
| 1.4 | Stap 2: scan en verbinden | Netwerken gesorteerd op signaal. Juist wachtwoord: "Verbonden", daarna STA weer uit | console, `/api/provision/state` |
| 1.5 | Fout WiFi-wachtwoord | "Onjuist WiFi-wachtwoord". Niets opgeslagen (NVS `ssid` ongewijzigd) | console |
| 1.6 | Verborgen SSID | Verbindt op naam | console |
| 1.7 | Telefoon valt weg bij kanaalwissel | Na opnieuw verbinden gaat de pagina verder | notitie per OS |
| 1.8 | Stap 3 uitlezen (afstandsbediening, geen TX) | Profiel opgeslagen | console |
| 1.9 | Stap 3 koppelen (**RF-TX, aparte toestemming**, unit in koppelmodus) | Eén aanbod, unit bevestigt, "gekoppeld". Mislukt: opnieuw te proberen, nooit automatisch herhaald | RF-console, `tx_starts` |
| 1.10 | Stap 4: sleutel maken | Duur gemeten (verwacht ~6–8 s). Laagste vrije heap; vingerafdruk | serieel, `/api/diag` |
| 1.11 | Download iOS, Android en captive-venster | Bestand opgeslagen, of de terugval werkt | schermafbeeldingen |
| 1.12 | Controle: verkeerde zin, ander bestand, juiste zin | `bad-passphrase` / `key-mismatch` / geregistreerd. Herstart naar het thuisnetwerk | console |
| 1.13 | Interop | `openssl pkey -in orcon-recovery-key.pem -pubout` geeft het punt waarvan de vingerafdruk overeenkomt | terminal |
| 1.14 | Onderbroken installatie: stroom eraf na stap 1, na stap 2 en tijdens stap 4 | Komt terug in de juiste stap. Aanmelden met het stap-1-wachtwoord. Geen open beheer | log |
| 1.15 | Setup via het thuisnetwerk (na stap 2, tijdens de test en daarna) | `curl http://<STA-IP>/api/provision/state` → 403 `setup-ap-only`. Ook een route naar 192.168.4.1 via het STA-IP → 403 | terminal |
| 1.16 | Geen geheimen in logs | Zoek in serial en console op wachtwoord, wachtwoordzin en PEM: niets | grep-uitvoer |

## Fase 2 — tweede board: herstel en fabrieksreset (toestemming: knoppen en resets)

| # | Test | Verwacht |
|---|---|---|
| 2.1 | Gewone PRG-druk | Alleen OLED-wake |
| 2.2 | Korte RST zonder PRG | Normale start, geen menu |
| 2.3 | PRG al ingedrukt tijdens RST | ROM-downloadmodus (scherm zwart). Een gewone RST herstelt dat |
| 2.4 | RST, dan PRG 3 s | Aftelling, daarna "geannuleerd" |
| 2.5 | RST, dan PRG 15 s; Annuleren kiezen | Menu, daarna niets gewijzigd |
| 2.6 | Herstelmodus, wachtwoord vergeten | AP-only, 15 min. Herstel met het sleutelbestand en een nieuw wachtwoord. Oude sessies afgemeld. Profielen, WiFi en koppeling behouden |
| 2.7 | Herstelmodus, nieuwe sleutel (aangemeld) | Nieuwe sleutel geregistreerd. Het oude bestand geeft `key-mismatch` |
| 2.8 | WiFi wissen | AP, aanmelden, nieuwe WiFi via de test. Wachtwoord en profielen behouden. `nvs.net80211` leeg |
| 2.9 | Fabrieksreset met een korte druk in de bevestiging | Geannuleerd, niets gewist |
| 2.10 | Fabrieksreset volledig | OLED "Fabrieksreset". Daarna staat New, met een nieuw AP-wachtwoord |
| 2.11 | Stroom eraf tijdens de reset (direct na het bevestigen, en tijdens het wissen) | Bij de volgende start wordt de reset afgemaakt. Daarna New. Nooit een half gewiste toestand met het oude wachtwoord |
| 2.12 | Opnieuw installeren na de reset | Wizard volledig. Nieuwe eigen RF-ID bij de eerste koppeling |
| 2.13 | Oude eigenaarssleutel na overdracht | Herstel met het oude bestand werkt niet. `auth-code` met de releasesleutel geeft 403 |
| 2.14 | Menu tijdens een OTA-proefstart | "Update nog niet bevestigd". Geen herstart, geen rollback |
| 2.15 | OTA en rollback na een reset | `.orconfw` via de pagina geïnstalleerd en bevestigd. `orcon.py ota-install --test-rollback` geeft PASS |
| 2.16 | OTA-weigeringen | Gewijzigde bundel → `bad-signature` / `hash-mismatch`. Oudere release → `older-release`. Zonder sessie → 401 |
| 2.17 | Geen ongeautoriseerde TX | Na alle geweigerde aanroepen is `tx_starts` gelijk. Het RF-budget wordt gerespecteerd |

## Fase 3 — het werkende board: migratie (toestemming per OTA)

Volg `PROVISIONING.md` §7. Na elke stap controleer je:
- profiel "Marco Huis" en de koppeling bestaan nog;
- aanmelden werkt;
- `/api/ota`: slots, `owner_model`, `release`;
- `orcon.py status`.

Eén bedieningscommando alleen met aparte RF-toestemming.

1. Eigen wachtwoord zetten met de huidige firmware (`auth-code`).
2. OTA van het nieuwe RX-only-image naar app0. Proefstart bevestigd. Staat `legacy`.
3. OTA van het nieuwe control-image naar app1 (CLI vraagt het wachtwoord). Bevestigd.
4. Herstelmodus, dan een sleutel registreren. Staat `done`. `auth-code` geeft nu 403 `owner-key-required`.

**Stopcriteria.** Bij elke afwijking stoppen. Terugval: de bootloader-rollback, of `heltec_v2.sh recover` (USB, aparte toestemming).
