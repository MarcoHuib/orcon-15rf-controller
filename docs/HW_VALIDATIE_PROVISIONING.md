# Plan: hardwarevalidatie van installatie, herstel en fabrieksreset

**Altijd**
- Geen flash, OTA, reset, RF-zending of nieuwe koppeling zonder expliciet akkoord per stap.
- Geen GitHub Actions.
- Bewijs komt lokaal in `build/` en `captures/` (git-ignored).
- Wat niet getest is, heet **NOT TESTED**.

**Uitgangspunt: er is misschien maar één Heltec V2.**
- Het werkende board wordt nooit automatisch gereset, geflasht of opnieuw gekoppeld.
- Elke handeling heeft daarom een klasse:
  - **S** (veilig op het werkende board);
  - **V** (kan de installatie verstoren, maar is herstelbaar);
  - **D** (vraagt een tweede board, of beschadigt de huidige installatie).

## 0. Al gedaan zonder hardware-ingreep (14-11-2026)

**Hosttests en bouw**
- Alle host-suites, CLI, de paginatests, broncode-veiligheid, privacy en beide flash-tooltests slagen.
- Alle 11 omgevingen bouwen.
- UBSan slaagt; ASan is NOT TESTED (hangt op deze Mac al vóór `main`).

**Simulaties**
- Knopbediening, fabrieksreset met een stroomonderbreking op elke stap, onderbroken installatie, onleesbare en dubbelzinnig opgeslagen gegevens, en een volledige wizard via de echte HTTP-handlers.

**Baseline van het werkende board** (alleen gelezen: `orcon.py status`, twee GET's):
- a5260ad control in app1 (`valid`, actief), app0 `valid`;
- nog geen eigendomsmodel;
- vrije heap 175 840 B, laagste 140 608 B.

## 1. Werkend board: migratie (klasse S, akkoord per OTA)

Volg [`PROVISIONING.md` §7](PROVISIONING.md):
0. Eigen wachtwoord zetten met de huidige firmware.
1. Nieuwe RX-only naar app0.
2. Nieuwe control naar app1.
3. Eigenaarssleutel registreren in de herstelmodus.

Profiel, koppeling, eigen ID en WiFi blijven in alle stappen behouden.

**Na elke stap controleren**
- profiel en koppeling bestaan nog;
- aanmelden werkt;
- `/api/ota` toont slots, `owner_model` en `release`.

## 2. Werkend board, na de migratie: niet-destructieve tests (klasse S)

| # | Controle | Hoe | Verwacht | Bewijs |
|---|---|---|---|---|
| S1 | **PRG/RST-procedure** | RST, PRG 15 s, menu doorlopen, **Annuleren** kiezen; daarna een gewone PRG-druk | Aftelling na 1 s, menu, "geannuleerd". Een korte druk wekt alleen het OLED. Niets gewijzigd | foto/video OLED, `/api/ota` |
| S2 | Vroeg loslaten | RST, PRG 3 s | Aftelling, daarna "geannuleerd" | video |
| S3 | PRG tijdens RST | PRG vasthouden en RST indrukken | ROM-downloadmodus (scherm zwart). Een gewone RST start normaal | notitie |
| S4 | **Herstelmodus** | menu → Herstelmodus | AP-only, OLED "Herstelmodus 15m". Na 15 min vanzelf terug op het thuisnetwerk. Tijdens de herstelmodus geen RF-bediening | OLED, console |
| S5 | **iPhone en Android: captive portal** | in de herstelmodus verbinden met `Orcon15RF-Setup` | De pagina opent vanzelf, of handmatig `http://192.168.4.1` | schermafbeeldingen per OS |
| S6 | **WiFi op echte hardware** | herstelmodus → aanmelden → WiFi → hetzelfde thuisnetwerk testen; dan een fout wachtwoord; dan een verborgen SSID als die beschikbaar is | Juist: verbonden, opgeslagen, herstart naar het thuisnetwerk. Fout: "Onjuist WiFi-wachtwoord", niets opgeslagen. Ook als de telefoon even wegvalt, gaat de pagina verder | console, `/api/provision/state` |
| S7 | **PEM downloaden en opnieuw importeren** | herstelmodus → Systeem → Herstelsleutel. Op iPhone en Android: download, kopieertekst, bestand weer kiezen, wachtwoordzin. Dit is migratiestap 3, dus de eerste sleutelregistratie | Geregistreerd. Verkeerde zin of ander bestand: geweigerd. Werkt het downloaden in het captive-venster niet, dan werkt de terugval wel | schermafbeeldingen, console |
| S8 | **PBKDF2-duur** | uit de console-regels van S7 ("herstelsleutel … aangemaakt in N ms", "sleutelbestand gecontroleerd in N ms") en van het aanmelden | Maken en controleren, verwacht ~6–8 s (50 000 rondes). Aanmelden ~1,3 s. Laagste heap tijdens de bewerking | console, `/api/diag` |
| S9 | Interop met OpenSSL | `openssl pkey -in orcon-recovery-key.pem -pubout` | De vingerafdruk klopt met wat het apparaat toont | terminal |
| S10 | OTA zonder sessie | `ota-install` met een fout wachtwoord | 401, niets geschreven | CLI-uitvoer |
| S11 | Setup via het thuisnetwerk | `curl http://<LAN-IP>/api/provision/state` | 403 `setup-ap-only` | terminal |
| S12 | Geen geheimen in logs | serial en console doorzoeken op wachtwoord, wachtwoordzin en PEM | niets | grep |
| S13 | Eigendomsmarkering na de migratie | serieel log van de eerste start: `[owner] state legacy (migrated)`; daarna `done` | geen melding "ownership sentinel not written". Profielen en koppeling intact | serieel log |

## 3. Werkend board: kan de installatie verstoren (klasse V, herstelbaar, apart akkoord)

| # | Controle | Risico | Herstel |
|---|---|---|---|
| V1 | **WiFi wissen** via het menu | thuis-WiFi weg tot je hem opnieuw instelt | via het AP aanmelden en WiFi opnieuw instellen |
| V2 | Wachtwoordherstel met het sleutelbestand | het beheerderswachtwoord verandert | nieuw wachtwoord |
| V3 | `orcon.py ota-install --test-rollback` | het board draait daarna (bewust) het vorige image | de nieuwe firmware opnieuw installeren |
| V4 | Menu tijdens een OTA-proefstart | geen; moet weigeren ("update nog niet bevestigd") | — |

## 4. Tweede board nodig, of beschadigt de installatie (klasse D)

**Waarom deze klasse.** Een fabrieksreset op het werkende board wist ook de koppeling. Opnieuw koppelen vraagt een RF-zending en de unit in koppelmodus (stroom van de unit eraf). Daarom horen deze tests op een tweede board, of op het werkende board alleen met apart akkoord voor zowel de reset als de nieuwe koppeling.

| # | Controle | Verwacht |
|---|---|---|
| D1 | **Volledige installatie via iPhone en via Android**, vanaf de factory-image | Vier stappen; stap 3 overslaan kan. Daarna "Klaar" en herstart naar het thuisnetwerk |
| D2 | Eerste start, en een onjuist of afwijkend wachtwoord in stap 1 | geweigerd; een tweede "eerste wachtwoord" 403 |
| D3 | **Onderbroken installatie**: stroom eraf na stap 1, na stap 2 en tijdens stap 4 | Terug in de juiste stap, aanmelden met het stap-1-wachtwoord, nooit een open beheerfunctie |
| D4 | Stap 3 koppelen (**RF-zending, apart akkoord**) | één aanbod, de unit bevestigt; mislukt: opnieuw proberen, nooit automatisch |
| D5 | **Fabrieksreset** (bevestiging met 5 s vasthouden; ook de annuleringen) | Staat New, nieuw AP-wachtwoord; koppeling, profielen, wachtwoord en sleutel weg |
| D6 | **Gevolgen voor A/B-OTA en rollback.** Vooraf en achteraf `/api/ota` en `heltec_v2.sh recover check` (USB, alleen lezen, apart akkoord) | Bootkeuze ongewijzigd. Een image van het nieuwe model in de andere sleuf blijft staan. Een image van vóór dit model is vervangen door een kopie van het actieve image, dus twee opstartbare images. Daarna een `.orconfw` installeren en `--test-rollback`: PASS |
| D7 | **Onderbroken reset**: stroom eraf direct na het bevestigen, en tijdens het wissen of kopiëren | Volgende start: OLED "Fabrieksreset bezig", de reset wordt afgemaakt, daarna New. Nooit het oude wachtwoord met een halve toestand |
| D8 | Oude eigenaarssleutel na overdracht | herstel met het oude bestand werkt niet; `auth-code` met de releasesleutel 403 |
| D9 | Opnieuw installeren na de reset | wizard volledig; nieuwe RF-ID bij de eerste koppeling |
| D10 | OTA-weigeringen | gewijzigde bundel → `bad-signature`/`hash-mismatch`; oudere release → `older-release` |
| D11 | **NVS-autoformat nabootsen**: alleen de NVS-partitie wissen (`esptool erase_region 0x9000 0x5000`, USB, apart akkoord) | OLED "Reset nodig (PRG)", staat `locked`: geen wizard, geen aanmelden, geen herstel. Na de PRG-fabrieksreset: New, en de wizard werkt |
| D12 | Eigendomsmarkering beschadigen (beide sectoren aan het eind van `spiffs` overschrijven, USB) | Fail closed: geen normale start; OLED "Opslag onleesbaar: RST, PRG 15 s vast = fabrieksreset". De knoppen werken: de fabrieksreset (met 5 s bevestiging) herschrijft de markering, wist alles, en daarna New. Zonder knopbediening blijft het apparaat in deze toestand; geen computer nodig. Stroom eraf tijdens die noodreset: de volgende start toont weer "Opslag onleesbaar" of maakt de reset af, nooit New met oude gegevens. Tijdens een onbevestigde update (proefstart): geen automatische herstart en geen reset; OLED "Opslag onleesbaar, update onbevestigd: RST = vorige versie" |

**Stopcriteria.** Bij elke afwijking stoppen en vastleggen. Terugval: de rollback van de bootloader, of `heltec_v2.sh recover` (USB, apart akkoord).
