# Migratie, eerste tests en fabrieksreset op de eigen Heltec

**Besluit van 14-11-2026**
- Er is één Heltec V2: het werkende board. Er komt geen tweede board.
- Elke firmware-installatie, USB-handeling, reset en RF-zending gebeurt alleen na een apart akkoord voor precies die stap.
- Een nieuwe koppeling met de Orcon na de fabrieksreset is geaccepteerd.
- HTTPS blijft een blokkade voor de eerste publieke release. Het hoort niet bij deze migratie.

**Firmware.** Commit `2edb423`: de code waarop de Codex-review ronde 7 GO (security) gaf. Gebouwd en verpakt in een schone release-worktree (`orcon-release-2edb423`), volgens [`LOCAL_USB_RELEASE.md`](LOCAL_USB_RELEASE.md).

| Image | Omgeving | Bestemming | SHA-256 |
|---|---|---|---|
| RX-only | `heltec_v2_sx1276` | app0 (stap C) | `a5f34920f6c6eb7921609cb3ac7f1f2e8ee8d6264967821bf337af6aaba8235b` (1 222 592 B) |
| control | `heltec_v2_sx1276_control` | app1 (stap E) | `efdc51b48b1ff8fb99c71ed2b23208775ef7833ea0d3bdd93d33e534bcc5a756` (1 090 800 B) |

## 1. Vooraf gecontroleerd (alleen lezen)

| Controle | Uitkomst |
|---|---|
| Firmware en beide sleuven | app1: a5260ad control, `valid`, actief. app0: `valid`, volgens het installatieregister a5260ad RX-only. Over USB positief vast te stellen in stap B |
| OTA-sleutel | Eén sleutel overal. De publieke sleutel van `~/.orcon/ota-signing-key.pem` (rechten 0600) is identiek aan die in de firmware op het board, in `main` en in `2edb423` (SPKI-SHA-256 `e71543f1…`) |
| Images en handtekeningen | Bewijsrun op `2edb423`: 90 van 90 checks PASS, schoon. Beide pakketten `VERIFIED`. Beide images zijn door de OTA-regels van de firmware zélf goedgekeurd (hosttest met de echte broncode): RX-only door a5260ad (stap C), control door 2edb423 (stap E). Een proefhandtekening met de sleutel van de Mac klopt met de publieke sleutel van het board |
| Beheerderswachtwoord | Er is een wachtwoord gezet. Of het jouw eigen wachtwoord is, kan alleen jij zien. Het board had nog het tijdelijke testwachtwoord, waarvan niemand het geheim meer heeft: zie stap 0 |
| Profiel en koppeling | Alleen te zien na aanmelden: stap A |
| Configuratieback-up | Alleen via USB. Het board hangt nu boven aan een USB-voeding en is niet met de Mac verbonden: stap B |
| Herstel via USB | `heltec_v2.sh recover` met de pakketten van a5260ad en 2edb423. De simulaties slagen. Het board moet daarvoor aan de Mac |

## 2. Volgorde en toestemmingen

Elke stap begint pas als de vorige klopt. Bij een afwijking stoppen we.

**Stap 0: je eigen wachtwoord (jij; geen firmwarewijziging)**
1. Op de Mac: `python3 tools/orcon.py auth-code`. Dat geeft een eenmalige code, 10 minuten geldig.
2. Open `http://orcon15rf.local` en kies **Wachtwoord vergeten**. Vul de code in en twee keer je nieuwe wachtwoord.
3. Je bent aangemeld. Het tijdelijke testwachtwoord werkt niet meer.

**Stap A: profiel en koppeling bekijken (jij)**
- Aangemeld: profiel "Marco Huis" bestaat en is gekozen.
- Druk op geen enkele bedieningsknop: dat zendt RF.

**Stap B: USB-back-up en sleufcontrole (akkoord; board naar de Mac)**
1. Board met USB aan de Mac. Een poort openen herstart de ESP32 naar de downloadmodus.
2. `LOCAL_ENV=heltec_v2_sx1276 tools/heltec_v2.sh recover check 2edb423`. Die controleert de bootloader, de partitietabel, otadata en beide sleuven tegen bekende images. Er wordt niets geschreven. Duur ongeveer 10 minuten.
3. NVS (0x9000, 20 KB) twee keer uitlezen; beide keren moet hij identiek zijn. Ook otadata uitlezen. Alles komt in `.flashtools/backup-<UTC>/` (git-ignored, 0700), met SHA-256.
4. `python3 tools/nvs_keys.py nvs.bin` controleert elke CRC en toont naamruimten en sleutels, zonder waarden. Verwacht: `orcon` (WiFi, eigen ID, unit), `orconprofiles`, `orconauth` en `nvs.net80211`.
5. Een gewone herstart; het board draait weer a5260ad uit app1.

Terugzetten kan later, en alleen met apart akkoord: NVS op 0x9000 schrijven geeft exact deze configuratie terug, inclusief profielen, koppeling, WiFi en wachtwoord.

**Stap C: OTA 1, RX-only naar app0 (akkoord)**
- In de release-worktree `orcon-release-2edb423`: `python3 tools/orcon.py ota-install --worktree . --env heltec_v2_sx1276 --yes`
- a5260ad vraagt hiervoor geen aanmelding. Na het schrijven volgt een proefstart. Na minstens 60 s gezond bevestigt het image zichzelf.
- **Verwacht:** `/api/ota` toont `2edb423`, `rx-only`, app0 `valid` en `owner_model`. Aanmelden werkt met je wachtwoord: de staat is **Legacy**. Het profiel bestaat nog.
- In deze tussenstand kan het board geen RF zenden. Bedien de Orcon zolang met de afstandsbediening.
- **Mislukt:** dan zie je `ROLLED BACK` of geen bevestiging, en draait het board vanzelf weer a5260ad control uit app1.

**Stap D: controle na OTA 1 (jij en ik)**
- Aanmelden werkt en het profiel bestaat.
- De console heeft geen foutmeldingen, de laagste heap is normaal en er komen RX-frames binnen.

**Stap E: OTA 2, control naar app1 (akkoord; jij typt het wachtwoord)**
- Jij voert dit uit in je eigen terminal, in de release-worktree, want het vraagt je webwachtwoord:
  `python3 tools/orcon.py ota-install --worktree . --env heltec_v2_sx1276_control --tx-ack "control firmware" --yes`
- **Verwacht:** `2edb423` control in app1, `valid`. In app0 staat `2edb423` RX-only als rollbackdoel. Beide sleuven hebben nu het eigendomsmodel.

**Stap F: controle na OTA 2**
- Hetzelfde als stap D.
- Ook de bedieningspagina toont het profiel. Nog steeds geen knop indrukken.

**Terug naar a5260ad (alleen als het nodig is; apart akkoord)**
- Tijdens een proefstart gaat dat vanzelf: een image dat zich niet bevestigt, rolt terug.
- Daarna weigert de nieuwe firmware bewust elke update naar firmware van vóór het eigendomsmodel. Terug kan dan alleen via USB:
  - in de release-worktree van a5260ad: `LOCAL_ENV=heltec_v2_sx1276 tools/heltec_v2.sh recover write a5260ad`;
  - daarna eventueel a5260ad control via OTA.
- De NVS blijft bruikbaar, want a5260ad negeert de nieuwe sleutels. De NVS-back-up van stap B zet de configuratie desnoods exact terug.

## 3. Eerste tests na de migratie (geen firmwarewijziging, geen RF)

| # | Test | Hoe | Verwacht | Bewijs |
|---|---|---|---|---|
| T1 | Herstelmenu openen en annuleren | RST, en binnen 20 s PRG 15 s vasthouden. Kies **Annuleren** | Aftelling, menu, "geannuleerd". Er verandert niets | foto of video van het OLED, `/api/ota` |
| T2 | Tijdelijke herstelmodus | menu → **Herstelmodus** | Herstart. Alleen `Orcon15RF-Setup`; OLED "Herstelmodus 15m" met het AP-wachtwoord. Het thuisnetwerk is uit (`orcon15rf.local` is weg). Na 15 minuten vanzelf terug | OLED |
| T3 | Directe WiFi met de iPhone | iPhone → WiFi → `Orcon15RF-Setup`, wachtwoord van het OLED | De pagina opent vanzelf, of handmatig in Safari via `http://192.168.4.1` | schermafbeeldingen |
| T4 | Echte interface | aanmelden met je wachtwoord; Systeem bekijken | Alles werkt op de iPhone. Er staan geen bedieningsknoppen op het setupnetwerk | schermafbeeldingen |
| T5 | PEM maken, downloaden en opnieuw importeren | Systeem → Herstelsleutel → wachtwoordzin (minstens 10 tekens). Daarna downloaden (Bestanden-app), het bestand weer kiezen en de zin invullen. Eerst één keer met een verkeerde zin | Een verkeerde zin wordt geweigerd. Daarna "geregistreerd": staat **Done**, en `orcon.py auth-code` geeft 403 `owner-key-required`. Dit is migratiestap 3 uit [`PROVISIONING.md` §7](PROVISIONING.md) | schermafbeeldingen; vingerafdruk |
| T6 | Werkelijke cryptotijden | De console tijdens T5: "herstelsleutel … aangemaakt in N ms", "sleutelbestand gecontroleerd in N ms" en "aangemeld (wachtwoordcontrole N ms)". Lees ze vóór het einde van de herstelmodus: een schermafbeelding van de console, of de Mac kort op het setupnetwerk, dan lees ik `/api/setup/console` | Verwacht ongeveer 6–8 s voor maken en controleren, en ongeveer 1,3 s voor aanmelden | console |
| T7 | Profiel en koppeling behouden | Aangemeld: het profiel bestaat. Het sterkste bewijs is de USB-dump van vóór de fabrieksreset, vergeleken met die van stap B: `nvs_keys.py --diff` | `orcon` en `orconprofiles` zijn ongewijzigd; `orconown` is nieuw | diff |

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
- Een verse USB-back-up vlak vóór de reset, zoals in stap B. Daarmee kun je terug naar de huidige configuratie.
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
| Terug naar de configuratie van vóór de reset | USB, apart akkoord: de NVS-back-up terugschrijven. Profielen, koppeling, WiFi, wachtwoord en sleutel komen terug, en de oude koppeling in de unit werkt weer |
| Board start niet meer | USB, apart akkoord: `LOCAL_ENV=heltec_v2_sx1276 tools/heltec_v2.sh recover write 2edb423` schrijft RX-only naar app0 en kiest app0; NVS blijft |

**Stopcriteria.** Bij elke afwijking stoppen en vastleggen. Er is geen enkele automatische RF-zending; elke zending vraagt een eigen akkoord.
