# Migratie, eerste tests en fabrieksreset op de eigen Heltec

> Historisch plan (november 2026). Het wachtwoordherstel met `orcon.py auth-code` en de staat Legacy zijn verwijderd. De migratie naar een eigen herstelsleutel staat in [`HERSTELSLEUTEL_MIGRATIE.md`](HERSTELSLEUTEL_MIGRATIE.md).

**Besluit van 14-11-2026**
- Er is één Heltec V2: het werkende board. Er komt geen tweede board.
- Elke firmware-installatie, USB-handeling, reset en RF-zending gebeurt alleen na een apart akkoord voor precies die stap.
- Een nieuwe koppeling met de Orcon na de fabrieksreset is geaccepteerd.
- HTTPS blijft een blokkade voor de eerste publieke release. Het hoort niet bij deze migratie.

**Besluit van 17-11-2026: volledig draadloos**
- De Heltec blijft bij de ventilator. USB alleen als het technisch nodig is: zie "Wat alleen met USB kan".
- Geen nieuwe USB-back-up. De NVS-dump van 12-11 is de referentie, zolang `orcon.py backup-check` op elk veld "gelijk" geeft.
- Alle bestaande back-ups blijven bewaard.
- Volgorde: eerst RX-only naar app0, daarna control naar app1. Het laatst geïnstalleerde image wordt het actieve. Zo eindigt control als actieve firmware, met RX-only als terugval.
- Eerst een proefupdate (`--test-rollback`), met een eigen akkoord.

**Firmware.** Commit `2edb423`: de code waarop de Codex-review ronde 7 GO (security) gaf. Gebouwd en verpakt in een schone release-worktree (`orcon-release-2edb423`), volgens [`LOCAL_USB_RELEASE.md`](LOCAL_USB_RELEASE.md).

| Image | Omgeving | Bestemming | SHA-256 |
|---|---|---|---|
| RX-only | `heltec_v2_sx1276` | app0 (stap B en C) | `a5f34920f6c6eb7921609cb3ac7f1f2e8ee8d6264967821bf337af6aaba8235b` (1 222 592 B) |
| control | `heltec_v2_sx1276_control` | app1 (stap E) | `efdc51b48b1ff8fb99c71ed2b23208775ef7833ea0d3bdd93d33e534bcc5a756` (1 090 800 B) |

## 1. Vooraf gecontroleerd (alleen lezen, via WiFi)

| Controle | Uitkomst |
|---|---|
| Firmware en beide sleuven | 17-11 via `/api/ota` en `/api/diag`: app1 = a5260ad control, `valid`, actief. app0 `valid`, volgens het installatieregister a5260ad RX-only. 3,5 dag in bedrijf, geen crashdump, ontvanger zonder herstarts. Bootloader en partitietabel zijn op 12-11 over USB gecontroleerd; een OTA schrijft ze nooit |
| OTA-sleutel | Eén sleutel overal. De publieke sleutel van `~/.orcon/ota-signing-key.pem` (rechten 0600) is identiek aan die in de firmware op het board, in `main` en in `2edb423` (SPKI-SHA-256 `e71543f1…`) |
| Images en handtekeningen | Bewijsrun op `2edb423`: 90 van 90 checks PASS, schoon. Beide pakketten `VERIFIED`. Beide images zijn door de OTA-regels van de firmware zélf goedgekeurd (hosttest met de echte broncode): RX-only door a5260ad (stap B/C), control door 2edb423 (stap E). Een proefhandtekening met de sleutel van de Mac klopt met de publieke sleutel van het board |
| Beheerderswachtwoord | Er is een wachtwoord gezet. Of het jouw eigen wachtwoord is, kan alleen jij zien. Het board had nog het tijdelijke testwachtwoord, waarvan niemand het geheim meer heeft: zie stap 0 |
| Configuratieback-up | Geen nieuwe. Referentie: de NVS-dump van 12-11 21:28 UTC, 43 sleutels, alle CRC's goed. Inhoud: WiFi, eigen RF-ID, unit-ID, profiel en OLED-stand. Niet erin: het webwachtwoord en het setup-AP-wachtwoord, beide van 13-11. Die zet je zo nodig opnieuw |
| Back-up tegen het board | 17-11 via WiFi: eigen RF-ID, unit-ID en OLED-stand zijn gelijk aan de dump. De koppeling van 13-11 gebruikt precies die RF-ID. Profiel, actief profiel en WiFi-naam: `backup-check` in stap A |
| Bewaarplaats | Alle bestaande back-ups (14 NVS-dumps, 2 otadata, 8 app0-images, leeslogs) staan ook in `~/Projects/_backups/orcon-heltec-backups-20261117/`, met `SHA256SUMS`. De map is 0700, de bestanden zijn 0600, en alles staat buiten git. De originelen in de release-worktrees blijven staan. De referentie is `orcon-release-9fbd3eb__nvs-after-ota/nvs-after-ota.bin`. Let op: een NVS-dump bevat het WiFi-wachtwoord leesbaar |

## 2. Volgorde en toestemmingen

Elke stap begint pas als de vorige klopt. Bij een afwijking stoppen we.

**Stap 0: je eigen wachtwoord (jij; geen firmwarewijziging)**
1. Op de Mac: `python3 tools/orcon.py auth-code`. Dat geeft een eenmalige code, 10 minuten geldig.
2. Open `http://orcon15rf.local` en kies **Wachtwoord vergeten**. Vul de code in en twee keer je nieuwe wachtwoord.
3. Je bent aangemeld. Het tijdelijke testwachtwoord werkt niet meer.

**Stap A: profiel en back-up controleren (jij)**
1. Aangemeld: profiel "Marco Huis" bestaat en is gekozen. Druk op geen enkele bedieningsknop: dat zendt RF.
2. In deze checkout: `python3 tools/orcon.py backup-check --nvs ~/Projects/_backups/orcon-heltec-backups-20261117/orcon-release-9fbd3eb__nvs-after-ota/nvs-after-ota.bin`. Het vraagt je webwachtwoord.
3. Verwacht: zes regels "gelijk": eigen RF-ID, unit-ID, profielen, actief profiel, WiFi-netwerk en OLED-stand. De uitvoer noemt alleen velden, nooit waarden.
4. Staat er "ANDERS", dan stoppen we. Jij beslist dan of de back-up nog volstaat, bijvoorbeeld na een bewuste profielwijziging sinds 12-11.

**Stap B: proefupdate (akkoord; jij typt het wachtwoord)**
- **Wat er gebeurt.** RX-only `2edb423` gaat naar app0 en start als proef. De CLI meldt zich daar aan en vraagt de firmware zichzelf niet te bevestigen (hold). 300 s na de start keert het board vanzelf terug naar a5260ad control in app1.
- **Commando**, vanuit deze checkout (de CLI met de controles hieronder), met de pakketten uit de release-worktree:
  `python3 tools/orcon.py ota-install --worktree ../orcon-release-2edb423 --env heltec_v2_sx1276 --test-rollback --yes`
- **Waarom de hold aankomt:**
  - De CLI controleert je wachtwoord eerst op de huidige firmware. Klopt het niet, dan stopt hij, en is er niets geschreven.
  - Lukt de hold niet meteen, door het netwerk of een drukke webserver, dan probeert de CLI het elke 5 s opnieuw zolang de proefstart loopt.
  - De firmware bevestigt zichzelf pas 60 s na de start, en alleen met WiFi en een werkende ontvanger. De hold komt naar verwachting binnen een halve minuut.
- **Tijdens de proef.** Open de webpagina pas als de CLI `rollback test: hold -> (200, …)` toont. Dan heb je ongeveer 4 minuten: aanmelden en het profiel bekijken, geen knoppen. Ik lees intussen de console en vergelijk de RF-ID's met de back-up.
- **Verwacht:** `ROLLED BACK` en daarna `ROLLBACK TEST PASS`. Het board draait dan weer a5260ad control uit app1.
- **Als de hold toch niet aankomt:** de CLI toont `WARNING: the hold did not take effect`. De proef is dan een echte installatie geworden, met hetzelfde resultaat als stap C. Ga verder met stap D.
- **Na de proef** is app0 ongeldig: het board heeft dan maar één geldig image. Daarom volgt stap C direct, met een eigen akkoord. Wil je toch stoppen, dan zet ik de oude toestand draadloos terug: eerst a5260ad RX-only naar app0, daarna a5260ad control naar app1, zoals op 13-11. Dat zijn twee akkoorden.
- **Stoppen:** aanmelden lukt niet, het profiel ontbreekt, een RF-ID wijkt af, of het board is na 8 minuten niet terug. In dat laatste geval: RST of de stroom eraf. Een onbevestigd image wordt bij elke herstart verlaten.

**Stap C: OTA 1, RX-only naar app0 (akkoord)**
- In de release-worktree `orcon-release-2edb423`: `python3 tools/orcon.py ota-install --worktree . --env heltec_v2_sx1276 --yes`
- a5260ad vraagt hiervoor geen aanmelding. Na het schrijven volgt een proefstart. Na minstens 60 s gezond bevestigt het image zichzelf.
- **Verwacht:** `/api/ota` toont `2edb423`, `rx-only`, app0 `valid` en `owner_model`. Aanmelden werkt met je wachtwoord: de staat is **Legacy**. `backup-check` geeft overal "gelijk".
- In deze tussenstand kan het board geen RF zenden. Bedien de Orcon zolang met de afstandsbediening.
- **Mislukt:** dan zie je `ROLLED BACK` of geen bevestiging, en draait het board vanzelf weer a5260ad control uit app1.

**Stap D: controle na OTA 1 (jij en ik)**
- `backup-check`: alles "gelijk".
- De console heeft geen foutmeldingen, de laagste heap is normaal en er komen RX-frames binnen.

**Stap E: OTA 2, control naar app1 (akkoord; jij typt het wachtwoord)**
- Jij voert dit uit in je eigen terminal, in de release-worktree, want het vraagt je webwachtwoord:
  `python3 tools/orcon.py ota-install --worktree . --env heltec_v2_sx1276_control --tx-ack "control firmware" --yes`
- **Verwacht:** `2edb423` control in app1, `valid` en actief. In app0 staat `2edb423` RX-only als rollbackdoel. Beide sleuven hebben nu het eigendomsmodel.

**Stap F: controle na OTA 2**
- Hetzelfde als stap D.
- Ook de bedieningspagina toont het profiel. Nog steeds geen knop indrukken.

**Wat alleen met USB kan (laatste redmiddel, apart akkoord)**
- **Terug naar a5260ad** nadat een nieuw image zich heeft bevestigd. De nieuwe firmware weigert bewust elke update naar firmware van vóór het eigendomsmodel. In de release-worktree van a5260ad: `LOCAL_ENV=heltec_v2_sx1276 tools/heltec_v2.sh recover write a5260ad`. De NVS blijft bruikbaar, want a5260ad negeert de nieuwe sleutels.
- **Een NVS-back-up exact terugzetten.** Na de migratie moet dan ook de eigendomsmarkering gewist worden (de laatste 8 KB van spiffs). De dump van 12-11 bevat geen wachtwoord, en zonder dat wissen meldt het board Locked.
- **Een board dat helemaal niet start:** `recover write 2edb423` zet RX-only in app0; de NVS blijft.

Draadloos of met de knoppen lukt wél:
- een mislukte proefstart: het board gaat vanzelf terug;
- "Opslag onleesbaar" tijdens een proefstart: RST of de stroom eraf;
- een vergeten wachtwoord in de Legacy-staat: `auth-code`;
- kwijte WiFi-instellingen: PRG-menu → WiFi wissen, daarna instellen via de iPhone;
- een fabrieksreset, met de wizard en een nieuwe koppeling.

## 3. Eerste tests na de migratie (geen firmwarewijziging, geen RF)

| # | Test | Hoe | Verwacht | Bewijs |
|---|---|---|---|---|
| T1 | Herstelmenu openen en annuleren | RST, en binnen 20 s PRG 15 s vasthouden. Kies **Annuleren** | Aftelling, menu, "geannuleerd". Er verandert niets | foto of video van het OLED, `/api/ota` |
| T2 | Tijdelijke herstelmodus | menu → **Herstelmodus** | Herstart. Alleen `Orcon15RF-Setup`; OLED "Herstelmodus 15m" met het AP-wachtwoord. Het thuisnetwerk is uit (`orcon15rf.local` is weg). Na 15 minuten vanzelf terug | OLED |
| T3 | Directe WiFi met de iPhone | iPhone → WiFi → `Orcon15RF-Setup`, wachtwoord van het OLED | De pagina opent vanzelf, of handmatig in Safari via `http://192.168.4.1` | schermafbeeldingen |
| T4 | Echte interface | aanmelden met je wachtwoord; Systeem bekijken | Alles werkt op de iPhone. Er staan geen bedieningsknoppen op het setupnetwerk | schermafbeeldingen |
| T5 | PEM maken, downloaden en opnieuw importeren | Systeem → Herstelsleutel → wachtwoordzin (minstens 10 tekens). Daarna downloaden (Bestanden-app), het bestand weer kiezen en de zin invullen. Eerst één keer met een verkeerde zin | Een verkeerde zin wordt geweigerd. Daarna "geregistreerd": staat **Done**, en `orcon.py auth-code` geeft 403 `owner-key-required`. Dit is migratiestap 3 uit [`PROVISIONING.md` §7](PROVISIONING.md) | schermafbeeldingen; vingerafdruk |
| T6 | Werkelijke cryptotijden | De console tijdens T5: "herstelsleutel … aangemaakt in N ms", "sleutelbestand gecontroleerd in N ms" en "aangemeld (wachtwoordcontrole N ms)". Lees ze vóór het einde van de herstelmodus: een schermafbeelding van de console, of de Mac kort op het setupnetwerk, dan lees ik `/api/setup/console` | Verwacht ongeveer 6–8 s voor maken en controleren, en ongeveer 1,3 s voor aanmelden | console |
| T7 | Profiel en koppeling behouden | Terug op het thuisnetwerk: `orcon.py backup-check --nvs <dump van 12-11>` | Zes keer "gelijk" | uitvoer van de check |

Bewaar het PEM-bestand en de wachtwoordzin goed, op twee plekken. Zonder die twee kan een vergeten wachtwoord alleen nog met een fabrieksreset worden opgelost.

## 4. Fabrieksreset en de volledige wizard (later, eigen akkoord)

**Het open punt: een onderbroken kopie (onderzocht)**
- **Wanneer kopieert de reset?** Alleen als de andere sleuf firmware van vóór het eigendomsmodel bevat. Die wordt dan overschreven met een kopie van het actieve image, zodat oude firmware het oude herstel nooit meer kan openen.
- **Stroom eraf tijdens die kopie:**
  - De resetmarker blijft staan, en bij de volgende start gaat de reset verder.
  - De half beschreven sleuf start nooit: de bootloader controleert elk image volledig.
  - De beveiliging blijft dus heel. Wel staat er tot de volgende update maar één opstartbaar image in het apparaat.
- **Gevolg voor updates:** geen. Een update schrijft altijd in die andere sleuf, en het actieve image blijft het rollbackdoel.
- **Wat er ontbreekt:** alleen de noodterugval van de bootloader als het actieve image zelf beschadigd raakt.
- **Op jouw board doet dit zich niet voor.** Na stap E bevatten beide sleuven het nieuwe model, en de reset laat de andere sleuf dan ongemoeid.
- **Daarom een harde voorwaarde:** voer de reset niet uit tussen OTA 1 en OTA 2. Dan zou de reset de oude control-firmware in app1 vervangen door een kopie van RX-only.
- **Algemene oplossing, voor later:** bij een reset ook een niet-opstartbare andere sleuf opnieuw vullen, met een nieuwe Codex-ronde. Niet nodig voor deze migratie.

**Voorwaarden voor de reset**
- Stap E en F zijn klaar. `/api/ota` toont `valid` voor beide sleuven, en het installatieregister bevat beide 2edb423-images.
- Er loopt geen OTA-proefstart. Anders weigert het menu.
- Vlak vóór de reset: `backup-check` tegen de dump van 12-11 geeft overal "gelijk". Een nieuwe USB-back-up maken we niet (besluit 17-11).
- Na de reset krijgt het board bewust een nieuwe, willekeurige RF-ID. De oude koppeling werkt dan niet meer, en opnieuw koppelen is geaccepteerd. Terug naar de oude koppeling kan alleen via USB (zie hieronder).
- De Orcon-unit is bereikbaar voor een nieuwe koppeling.

**Verloop**
1. RST, PRG 15 s → **Fabrieksreset** → 5 s vasthouden om te bevestigen. Het OLED toont "Fabrieksreset", daarna "bezig: niet uitschakelen".
2. Het board wist wachtwoord, sleutel, profielen, koppeling, WiFi en ID. Daarna start het als nieuw: OLED "Installatie", `Orcon15RF-Setup` met een nieuw wachtwoord.
3. De wizard op de iPhone:
   - wachtwoord;
   - thuis-WiFi testen;
   - profiel: koppelen is een **RF-zending met apart akkoord op dat moment**, met de unit in koppelmodus; overslaan kan ook;
   - PEM downloaden en importeren;
   - klaar.
4. Normaal bedienen. Elke bediening is een RF-zending: alleen met akkoord.

**Stroom eraf**
- Tijdens de reset: bij de volgende start gaat de reset verder (OLED "Fabrieksreset bezig"), daarna New.
- Tijdens de wizard: terug in de juiste stap, en aanmelden met het wachtwoord van stap 1.

**Herstelpaden**
| Situatie | Herstel |
|---|---|
| OLED "Opslag onleesbaar" | PRG-fabrieksreset (noodpad) |
| Wizard of WiFi lukt niet | Het setupnetwerk blijft aan; opnieuw proberen, of menu → WiFi wissen |
| Terug naar de oude koppeling | Alleen via USB, apart akkoord (laatste redmiddel): de dump van 12-11 terugschrijven en de eigendomsmarkering wissen. De oude RF-ID, het profiel en WiFi komen terug, dus de unit herkent het board weer. Wachtwoord en herstelsleutel zitten niet in die dump: het board start zonder eigenaar, en je stelt ze opnieuw in. Niet getest |
| Board start niet meer | USB, apart akkoord: `LOCAL_ENV=heltec_v2_sx1276 tools/heltec_v2.sh recover write 2edb423` schrijft RX-only naar app0 en kiest app0; NVS blijft |

**Stopcriteria.** Bij elke afwijking stoppen en vastleggen. Er is geen enkele automatische RF-zending; elke zending vraagt een eigen akkoord.
