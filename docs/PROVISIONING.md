# Plug-and-play installatie, eigendom en herstel (ontwerp en beveiliging)

Dit document beschrijft hoe een nieuwe Heltec V2 zonder computer wordt ingericht, hoe eigendom werkt en hoe herstel en een fabrieksreset verlopen. Het licht ook de gemaakte keuzes toe.

- Gebruikershandleiding: [`INSTALLATIE.md`](INSTALLATIE.md).
- Releases en GitHub: [`RELEASES.md`](RELEASES.md).
- Hardwarevalidatie: [`HW_VALIDATIE_PROVISIONING.md`](HW_VALIDATIE_PROVISIONING.md).

**Code**
- Regels, host-getest: `src/provision.*`, `src/owner_key.*`, `src/reset_gesture.h`.
- Apparaatlijm: `src/owner_store.cpp`, `src/net.cpp`, `src/setup_web.cpp` en `src/ota_signed.cpp`.
- Pagina: `src/setup_page.html`.
- Alles staat achter `ORCON_OWNER_MODEL`. Dat geldt voor elke Heltec V2-build met de weblogin en de setup-UI: het control-image, het RX-only-rollbackimage en de webctl- en rftest-images.

## 1. Twee sleutels met elk één taak

| | Releasesleutel (uitgever) | Eigenaarssleutel (per apparaat) |
|---|---|---|
| Type | ECDSA P-256 | ECDSA P-256 |
| Privé-deel | Alleen bij de uitgever (nu `~/.orcon/ota-signing-key.pem`) | Alleen bij de eigenaar, in `orcon-recovery-key.pem`, versleuteld met een eigen wachtwoordzin |
| Publiek deel | In de firmware gecompileerd (`src/ota_pubkey.h`), gelijk voor iedereen | Alleen in de NVS van dat ene apparaat (`orconown/pub`) |
| Bewijst | "Dit image is officieel" | "Ik ben de eigenaar van dít apparaat" |
| Mag | Firmware-manifesten ondertekenen | Een vergeten wachtwoord herstellen of de sleutel vervangen, alleen in de fysieke herstelmodus |
| Mag niet | Toegang tot een apparaat geven. Geen wachtwoordherstel (behalve op een gemigreerd Legacy-apparaat zonder eigenaarssleutel, zie §7), geen installatie zonder de sessie van de eigenaar | Firmware ondertekenen |

**Waarom tekent de eigenaarssleutel geen OTA-images?**
- Een gebruiker compileert of ondertekent zelf niets.
- Een officiële update moet op elk apparaat te vertrouwen zijn zonder per-apparaat-handtekening. Daarvoor dient de releasesleutel.
- Toestemming om te installeren is een tweede, aparte vraag. Die beantwoordt de aangemelde eigenaar: elke OTA-stap (nonce, begin, upload, hold) eist sessie en CSRF-token.
- Zo heeft geen van beide sleutels genoeg aan zichzelf:
  - een gestolen releasesleutel geeft zonder het wachtwoord van de eigenaar geen toegang tot zijn apparaat;
  - een eigenaarssleutel kan geen firmware vervangen.

**Niet gebouwd: "beheer van vertrouwde firmwaresleutels".** Bijvoorbeeld een eigenaarssleutel die een eigen releasesleutel toevoegt voor een fork. Dat kan later met dezelfde verificatiecode, maar het vergroot het aanvalsoppervlak en is nu niet nodig.

**In de publieke firmware staat geen gedeeld geheim**
- De firmware bevat alleen de publieke releasesleutel.
- Het setup-wachtwoord, de beheerderswachtwoorden, de sessies en de eigenaarssleutels zijn per apparaat.

## 2. Installatiestaten

| Staat (NVS `orconown/state`) | Betekenis | WiFi |
|---|---|---|
| New (afwezig, geen wachtwoord) | eerste start of na een fabrieksreset | alleen setup-AP |
| Setup (1) | wachtwoord gezet, wizard bezig | alleen setup-AP; STA alleen tijdens de WiFi-test |
| Done (2) | eigenaarssleutel geregistreerd | normaal; AP-fallback als het thuisnetwerk ontbreekt |
| Legacy (3) | gemigreerd van firmware van vóór dit model | als Done; de oude ondertekende `auth-code` blijft tot er een eigenaarssleutel is |

**Schrijfvolgorde.** Stap 1 schrijft eerst `state=1` en pas dán het wachtwoord. Een wachtwoord zonder staat kan dus alleen van oude firmware komen, en alleen dat wordt Legacy. Een stroomonderbreking tijdens stap 1 laat nooit een toestand achter waarin het oude herstelpad (via de releasesleutel) opengaat.

**Fail closed**
- Een onleesbare NVS telt als Done zonder sleutel: geen wizard en geen herstel via de releasesleutel.
- Een onbekende waarde telt ook als Done.
- Een opgeslagen wachtwoord dat niet te lezen is, telt als "gezet", nooit als "geen". Stap 1 gaat dan niet opnieuw open (Codex OWNER-02); aanmelden meldt "niet beschikbaar".
- Na de registratie van een eigenaarssleutel vervallen openstaande herstelcodes van de releasesleutel. Zo'n code wordt daarna ook geweigerd (Codex OWNER-03).
- **Dubbelzinnige schrijfacties** (wel opgeslagen, maar het teruglezen mislukt) worden voorzichtig behandeld (Codex OWNER-06/07):
  - Na een mislukte opslag neemt het RAM over wat er werkelijk in de opslag staat.
  - Stap 1 opent alleen als de opslag op dat moment bevestigt dat er nog geen wachtwoord is.
  - Herstelcodes van de releasesleutel vervallen al vóór de eerste schrijfactie van een registratie, en het herstelpad sluit dan ook in RAM.
  - Bij het opstarten betekent een opgeslagen (of onleesbare) eigenaarssleutel altijd Done, wat de staat ook zegt.

## 3. Eerste installatie via de directe WiFi-verbinding

**Het setup-accesspoint `Orcon15RF-Setup`**
- WPA2-PSK met een willekeurig wachtwoord per apparaat.
  - 12 tekens uit 31 symbolen, ongeveer 59 bits.
  - Van de hardware-RNG.
  - Alleen op het OLED, nooit via serial, HTTP of MQTT.
- Wordt bij een fabrieksreset opnieuw gemaakt.
- Wie het wachtwoord heeft, stond dus bij het apparaat (fysieke aanwezigheid).

**De wizardroutes (`/api/provision/*`, regels in `provision::allow`) eisen alle drie:**
1. **Binnengekomen op het setup-AP.** Gecontroleerd op de socket, niet op headers:
   - het lokale adres is het AP-adres;
   - de client zit in het AP-subnet.
   lwIP gebruikt het weak host model: een host in het thuisnetwerk die een route naar 192.168.4.1 via ons STA-adres legt, bereikt het AP-adres wel. Maar zijn bronadres zit dan niet in het AP-subnet. Een vervalst AP-bronadres krijgt zijn antwoorden aan de AP-kant en kan geen TCP-verbinding opbouwen.
2. **Station down.** Tijdens installatie en herstel draait alleen het AP (`WIFI_AP`). Op dat moment bestaat er dus geen pad naar het thuisnetwerk. Alleen tijdens de WiFi-test is de STA kort actief. Dan zijn alle geheim-dragende routes dicht; alleen de statusvraag werkt.
3. **Een expliciete installatiemodus**: staat New/Setup, of de herstelmodus die alleen het PRG-menu kan starten (§4).

**Nooit:** een HTTP-route die een fabrieksreset start of het apparaat terugzet naar New. De host-tests controleren dat.

**Stap 1 — beheerderswachtwoord**
- 8 tot 64 tekens. Sterktemeter in de pagina.
- Opslag: PBKDF2-HMAC-SHA256 met 10 000 rondes en een salt per apparaat.
- Sessies en CSRF zoals in `docs/CONTROL.md`.
- **De overdracht.** HTTPS en Web Crypto zijn op `http://192.168.4.1` niet beschikbaar en niet nodig. Het wachtwoord gaat over de WPA2-versleutelde link van het setup-AP, waarvan alleen iemand bij het OLED de sleutel kent.
- **Restrisico.** Wie het AP-wachtwoord kende (het OLED zag) én de handshake opnam, kan dat verkeer later ontsleutelen.
- Stap 1 werkt maar één keer. Daarna vraagt elke wizardstap de sessie.

**Stap 2 — WiFi**
- Scannen (gesorteerd op signaal), of een naam typen voor een verborgen netwerk (directed probe).
- Wachtwoord tonen of verbergen.
- `POST /api/provision/wifi` start een niet-blokkerende test, terwijl het AP aan blijft.
- Pas na een gelukte verbinding wordt het netwerk opgeslagen. Daarna gaat de STA weer uit.
- Duidelijke meldingen: onjuist wachtwoord (reden 15/202/204/23), niet gevonden (201), mislukt (time-out).
- Alleen 2,4 GHz.
- Het AP volgt het kanaal van het thuisnetwerk. Een telefoon kan daardoor even wegvallen; de pagina legt dat uit en gaat verder zodra hij terug is.
- Werkt een netwerk later niet meer, dan start de AP-fallback (bestaand gedrag).

**Stap 3 — profiel**
- Het bestaande profielbeheer: afstandsbediening uitlezen, profiel opslaan, koppelen met de eigen Heltec-identiteit.
- Tijdens de installatie werken deze routes via het AP. Er is dan geen ander netwerk.
- Alle RF-limieten blijven gelden:
  - het budget van 10/min en 60/u;
  - één druk is één aanbod, zonder automatische herhaling.
- De eigen zender-ID wordt pas bij de eerste koppelpoging gemaakt, willekeurig. Daardoor krijgt de controller na een fabrieksreset ook een nieuwe ID.
- De stap is over te slaan ("Later doen") als de unit nu niet bereikbaar is.
- Een mislukte koppeling herhaal je met een nieuwe druk; je hoeft niet opnieuw te installeren.

**Stap 4 — persoonlijke herstelsleutel**

*Waar de sleutel ontstaat.* Op het apparaat, met mbedTLS en de hardware-RNG. Op `http://192.168.4.1` is de pagina geen secure context: `crypto.subtle` bestaat daar niet. Eigen of meegeleverde JS-cryptografie wilden we niet.

*Het sleutelbestand*
- Een standaard versleutelde PKCS#8 PEM:
  - PBES2;
  - PBKDF2-HMAC-SHA256 met 50 000 rondes;
  - AES-256-CBC;
  - salt en IV per bestand.
- OpenSSL en Python lezen het.
- De lezer weigert HMAC-SHA1, AES-128, minder dan 2048 rondes, onversleutelde sleutels en andere curves.
- Het publieke punt wordt uit de privésleutel berekend, nooit uit het bestand vertrouwd.

*Het verloop*
1. De wachtwoordzin (10 tot 64 tekens, niet gelijk aan het beheerderswachtwoord) gaat over het WPA2-AP naar het apparaat.
2. Het apparaat maakt de sleutel en versleutelt hem. Daarna wist het de privésleutel direct.
3. In RAM blijven alleen het publieke punt en het versleutelde bestand, voor een nieuwe download.
4. Download (Blob) en een kopieertekst als terugval. In het captive-portal-venster werken downloads vaak niet; daarom staat er ook "open http://192.168.4.1 in Safari of Chrome".
5. **Afronden** kan alleen na een controle: de eigenaar levert het opgeslagen bestand weer aan (bestand of plaktekst) met de wachtwoordzin.
6. Pas als dat ontsleutelt naar exact het wachtende publieke punt, wordt de publieke sleutel geregistreerd (`orconown/pub`) en volgt staat Done. Daarna herstart het apparaat naar het thuisnetwerk.

*Bewezen en niet bewaard*
- Dit bewijst dat het bestand compleet is en dat de wachtwoordzin klopt.
- De privésleutel komt nooit in NVS, Git of logs. De console logt alleen de vingerafdruk.

**Captive portal (onderzocht)**
- Er draait een DNS-server die elke naam naar het AP verwijst. Elke vreemde Host krijgt een 302 naar `http://192.168.4.1/`.
- Gevolg: iOS (`captive.apple.com`) en Android (`generate_204`) openen de wizard vanzelf.
- Beperkingen van die mini-browsers: geen of beperkte downloads en bestandskeuze, en cookies alleen per venster. Op iOS sluit het venster als de telefoon het netwerk kwijtraakt.
- Opvang:
  - de wizardstaat staat op het apparaat, dus na opnieuw aanmelden ga je verder;
  - stap 4 heeft de kopieertekst en de verwijzing naar de gewone browser;
  - `http://192.168.4.1` handmatig openen werkt altijd.

## 4. Fysieke bediening: herstelmenu op PRG

**De hardware**
- RST is de EN-pin van de ESP32: een harde reset, niet leesbaar voor software.
- PRG is GPIO0, de bootstrap-pin. Ingedrukt op het moment dat RST loskomt, start de ROM-downloadmodus: de firmware draait niet en het scherm blijft zwart. Kort op RST drukken lost dat op.
- Daarom: eerst RST loslaten, dan pas PRG. Daarna is GPIO0 een gewone ingang; de firmware leest hem alleen met een pull-up.

**Bediening (`src/reset_gesture.h`, 32 hosttests)**
1. Druk kort op RST.
2. Houd binnen 20 s na de start PRG 15 s vast. Na 1 s verschijnt een aftelling op het OLED.
   - Ook tijdens de WiFi-wachttijd bij het opstarten.
   - Een gewone korte druk wekt alleen het OLED, zoals altijd.
3. Het menu opent. Laat PRG los. Kort drukken = volgende optie, 2 s vasthouden = kiezen.
4. Opties, in deze volgorde:
   - **Annuleren** (standaard);
   - **Herstelmodus**;
   - **WiFi wissen**;
   - **Fabrieksreset**.
5. Een fabrieksreset vraagt een aparte bevestiging: loslaten, dan opnieuw 5 s vasthouden. Een korte druk annuleert.

**Bescherming tegen per ongeluk**
- Elke stap vraagt een nieuwe druk na loslaten. Een vastzittende knop kiest nooit iets.
- Na een minuut zonder invoer sluit het menu.
- Het opstartvenster is vergrendeld: het opent niet opnieuw na de wrap van `millis()`.
- Tijdens een OTA-proefstart weigert het menu alle acties. Een herstart zou de update anders eerst terugdraaien.

**Drie niveaus**

| | Herstelmodus (wachtwoordherstel) | WiFi wissen (netwerkherstel) | Fabrieksreset (eigendomsoverdracht) |
|---|---|---|---|
| Wist | niets | thuis-WiFi, ook de kopie in de WiFi-driver (`nvs.net80211`) | alles hieronder |
| Behoudt | alles | wachtwoord, sleutel, profielen, koppeling, eigen ID, firmware | firmware, bootloader, partitietabel, OTA/rollback, releasesleutel |
| Daarna | setup-AP, 15 minuten, één boot | setup-AP; aanmelden en de WiFi-test | staat New: de wizard opnieuw |
| Gebruik | vergeten wachtwoord met het sleutelbestand; een nieuwe sleutel (aangemeld); een Legacy-apparaat een sleutel geven | verhuizen, nieuwe router | verkopen, overdragen, alles opnieuw |

**Herstelmodus**
- Dit is een RTC-vlag voor één boot. Een stroomonderbreking annuleert hem.
- Wachtwoord vergeten: `POST /api/provision/recover` met het sleutelbestand, de wachtwoordzin en een nieuw wachtwoord.
  - Telt mee in de bestaande aanmeldblokkade.
  - Na succes worden alle sessies afgemeld.
- Een nieuwe sleutel maken kan alleen aangemeld én in deze modus. Een gestolen sessie kan de sleutel dus niet vervangen.

**Fabrieksreset (`provision::resume`, met een stroomonderbreking getest op elke stap)**
1. De bevestigde knopbediening schrijft alleen een marker (NVS `orconreset`) en herstart.
2. Bij de volgende start, vóór alles wat NVS leest, wist het apparaat in deze volgorde:
   1. `orconauth` (wachtwoord);
   2. `orconown` (eigenaarssleutel en staat). Na het wachtwoord: een reset die hiertussen stopt, laat een geïnstalleerd apparaat zonder wachtwoord achter (op slot), nooit "wachtwoord zonder staat" (zoals Legacy);
   3. `orconprofiles`;
   4. `orcon` (WiFi, unit, eigen ID, AP-wachtwoord, MQTT, OLED);
   5. `nvs.net80211`.
3. Daarna:
   - de crashdump (die kan RAM-inhoud bevatten);
   - een image in de andere OTA-sleuf dat van vóór dit model is (§6);
   - als laatste de marker.
4. Daarna een schone herstart als nieuw apparaat.

Bij een stroomonderbreking gaat de reset bij de volgende start verder. Zonder marker gebeurt niets. Is de marker onleesbaar, dan start het apparaat niet normaal: het toont een melding en probeert het over 10 s opnieuw. Onleesbaar betekent nooit "geen reset" (Codex OWNER-01). Een crash, update of stroomonderbreking kan dus nooit zelf een reset veroorzaken, en een gestarte reset wordt altijd afgemaakt.

**Intrekking van de vorige eigenaar**
- Zijn publieke sleutel is weg, dus zijn sleutelbestand verifieert niets meer.
- Sessies stonden alleen in RAM.
- Het AP-wachtwoord is nieuw.
- Het herstelpad via de releasesleutel gaat niet meer open: daarvoor is een wachtwoord van oude firmware nodig, en dat is gewist.
- De eigen RF-ID wordt bij de volgende koppeling nieuw en willekeurig.

**Grens.** De Orcon-unit bewaart zelf de oude koppeling met de oude ID. Die kan alleen op de unit worden gewist (unit-reset volgens Orcon). Na de reset zendt de Heltec nooit meer met die ID.

## 5. Firmware-updates zonder USB

**v1 "targeted" (het bestaande ontwikkelaarspad, `orcon.py ota-install`)**
- Voor één bord: MAC en eenmalige nonce.

**v2 "release" (nieuw)**
- Eén handtekening voor alle borden:
  `{"v":2,"kind":"orcon-release","env","variant","allow_tx","commit","version","size","sha256"}`
- Het apparaat:
  - controleert de handtekening tegen de releasesleutel;
  - weigert een oudere release dan de draaiende (`ORCON_RELEASE_VERSION`);
  - weigert het RF-testimage.
- De eigenaar installeert een `.orconfw`-bundel in Systeem → Firmware.

**Voor beide vormen geldt**
- De sessie van de eigenaar is verplicht.
- Alle bestaande controles blijven: grootte, hash, commit, varianten en markers, `esp_ota_end`, proefstart en rollback.
- Nieuw is de marker `ORCON-OWNER-MODEL v1`. Firmware van vóór dit model kan niet meer via OTA terugkomen, want het herstelpad daarvan vertrouwt de releasesleutel.

`tools/orcon.py`:
- `ota-install` vraagt op apparaten met dit model het webwachtwoord.
- `release` maakt de bundel. Het weigert een image dat niet als die release is gebouwd; het stempel `ORCON-RELEASE N;` staat in het image.

## 6. Rollback en oude firmware

Een image van vóór dit model in de andere sleuf zou na een terugval het oude herstel via de releasesleutel weer openen. De maatregelen:

1. OTA installeert alleen images met de owner-model-marker.
2. Een fabrieksreset wist zo'n image uit de andere sleuf. Een image van het nieuwe model blijft staan als rollbackdoel.
3. Na de migratie in twee stappen (§7) bevatten beide sleuven het nieuwe model.

**Wat overblijft**
- USB-herstel: `heltec_v2.sh recover` zet bewust het RX-only-pakket terug. Dat is fysieke toegang; zie §8.
- Anti-rollback via eFuses (`secure_version`) gebruiken we niet. Dat vraagt eenmalig, onomkeerbaar eFuses branden en een andere bootloader.

## 7. Migratieplan voor bestaande installaties

**Situatie op 14-11-2026**
- app1: a5260ad control (actief).
- app0: a5260ad RX-only.
- NVS: thuis-WiFi, eigen ID, unit, profiel, en het tijdelijke testwachtwoord.

**Wat verandert en wat blijft**
- **NVS-configuratie, apparaatidentiteit, koppeling en profielen:** blijven ongewijzigd. De nieuwe firmware leest dezelfde sleutels.
- **Beheerderswachtwoord:** blijft. Een wachtwoord zonder staat wordt bij de eerste start Legacy.
- **De ingebouwde publieke OTA-sleutel:** wordt de releasesleutel. Hij tekent nog steeds firmware, maar geeft geen wachtwoordherstel meer zodra er een eigenaarssleutel is.
- **A/B en rollback:** ongewijzigd. Let op de volgorde, zodat de rollbacksleuf het nieuwe model krijgt.

**Stappen (elke installatie apart, alleen met toestemming)**
0. Zet eerst een eigen wachtwoord, nog met de huidige firmware: `orcon.py auth-code` en de webinterface. Het apparaat heeft nu een tijdelijk testwachtwoord.
1. OTA het nieuwe RX-only-image naar app0. De huidige firmware accepteert dat zonder aanmelden, zoals nu. Het nieuwe image start als Legacy, met WiFi, en bevestigt zichzelf. Rollbackdoel blijft a5260ad control.
2. OTA het nieuwe control-image naar app1. `orcon.py` vraagt nu het webwachtwoord. Daarna bevatten beide sleuven het nieuwe model.
3. Registreer de eigenaarssleutel. Dat vraagt fysieke toegang: RST → PRG → Herstelmodus → telefoon op `Orcon15RF-Setup` → aanmelden → Systeem → Herstelsleutel. Daarna staat Done en is het herstel via de releasesleutel uit.

Profielen, koppeling, ID en WiFi blijven in alle stappen. De andere sleuf terugzetten met `heltec_v2.sh recover` blijft mogelijk; dat schrijft het RX-only-pakket van de gekozen commit.

## 8. Wat niet veilig of niet haalbaar is

Deze punten zijn bewust niet als oplossing gebouwd.

**Door de browser vertrouwde HTTPS op het LAN, zonder cloud of handwerk**
- Niet haalbaar. Zie §9.
- Gevolg: op het thuisnetwerk is het verkeer platte HTTP. Wie daar kan afluisteren (bijvoorbeeld met ARP-spoofing), kan wachtwoord of sessiecookie onderscheppen (Codex SEC-04).
- Beperking: alles wat sleutelmateriaal raakt, gaat alleen over het WPA2-setup-AP met station uit.

**Cryptografie in de browser op `http://`**
- Geen secure context, dus geen `crypto.subtle`.
- Niet zelf gebouwd. De sleutel ontstaat op het apparaat.

**Fysieke aanvaller met USB of flash-toegang**
- Er is geen flash-encryptie en geen secure boot. Die vragen eFuses, en dat is onomkeerbaar; daarvoor is een eigen besluit nodig.
- Wie het bord meeneemt, kan NVS lezen: wachtwoord-hash en publieke sleutel, nooit de privésleutel.
- Diegene kan ook eigen firmware flashen.
- Een fabrieksreset kan iedereen met fysieke toegang uitvoeren. Dat is gewenst: het is de overdrachtsprocedure.

**Offline aanval op het setup-AP**
- Met ongeveer 59 bits is een opgenomen WPA2-handshake niet praktisch te kraken.
- Het risico bestaat wel, en alleen tijdens installatie en herstel.

**De unit zelf ontkoppelen**
- RAMSES-II heeft daar geen commando voor dat wij kunnen sturen. Zie §4.

**POST-bodies in de Arduino-WebServer (Codex OWNER-05, geërfd, niet opgelost)**
- De WebServer van Arduino-ESP32 2.0.16 leest een POST-body helemaal in (`malloc` van Content-Length) vóórdat een handler draait.
- Een client in het LAN of op het AP kan dus met een grote of heel langzame body geheugen en tijd vasthouden. De handler-limieten (bijvoorbeeld 1900 B) komen pas daarna.
- Dit geldt voor elke bestaande POST-route, ook op `main`.
- Een oplossing vraagt een aangepaste of andere webserver met limieten vóór het bufferen. Dat is een aparte wijziging, buiten deze opdracht.
- Gevolg: hooguit een DoS (geheugen, herstart). Geen toegang.

**Hardware**
- Pas na hardwarevalidatie bewezen: de crypto op de ESP32 zelf en de duur van PBKDF2 (50 000 rondes, ongeveer 6 s geschat), het werkelijke knopgedrag, het kanaalwissel-gedrag van het AP en de captive portal per telefoon.
- Zie [`HW_VALIDATIE_PROVISIONING.md`](HW_VALIDATIE_PROVISIONING.md). Tot die tijd: **NOT TESTED**.

## 9. HTTPS en provisioning voor publieke distributie (onderzoek)

**Wanneer vertrouwt een browser HTTPS zonder waarschuwing?**
1. Het certificaat hoort bij een publieke DNS-naam;
2. van een publieke CA;
3. of de gebruiker installeert zelf een eigen root (CA).

Een LAN-IP of `.local` krijgt geen publiek certificaat.

| Optie | Werkt zonder waarschuwing | Afhankelijkheid | Oordeel |
|---|---|---|---|
| Per apparaat een publieke naam en ACME (DNS-01) via de uitgever (zoals `*.plex.direct`) | ja | een cloud- en DNS-dienst van de uitgever, die altijd bereikbaar moet zijn; registratie per apparaat; de privacy van de naam | niet zonder eigen infrastructuur; buiten scope |
| Self-signed certificaat per apparaat (op de ESP32 gemaakt) | nee: elke browser waarschuwt, op iOS bij elk bezoek | geen | traint gebruikers om waarschuwingen weg te klikken; geen echte server-authenticatie |
| Eigen CA per huishouden, op elke telefoon geïnstalleerd | ja, na installatie | gebruikersactie per telefoon (iOS-profiel, Android-instellingen) | te technisch voor de doelgroep |
| Web Crypto op `http://` | n.v.t. | alleen `localhost` telt als secure context | niet mogelijk |
| **Gekozen: geheimen alleen over het WPA2-setup-AP (OLED-wachtwoord, station uit); op het LAN HTTP met sessie, CSRF, Host- en Origin-controle** | n.v.t. | geen; geen Mac, geen cloud | haalbaar en eerlijk over het restrisico |

**Kosten van TLS op het apparaat.** `esp_https_server` met mbedTLS vraagt ongeveer 40–50 KB heap per verbinding. De laagste vrije heap is nu ongeveer 160 KB. Eén verbinding is waarschijnlijk haalbaar (NOT TESTED). Zonder vertrouwd certificaat lost het echter niets op.

**Geen Mac meer nodig.** Het eerste wachtwoord, het herstel en de updates lopen via de telefoon. De Mac blijft alleen het ontwikkelaarspad: `ota-install`, en `auth-code` voor Legacy.

## 10. Testbewijs (lokaal, host)

| Suite | Wat |
|---|---|
| `provision` (98) | staten, migratie, routebeleid, fabrieksreset met een stroomonderbreking op elke stap, onleesbare marker, vergrendelde herstelmodus-timer |
| `reset_gesture` (32) | gewone druk, korte reset, lange druk, annuleren, alle menukeuzes, bevestiging, vastzittende knop, dender, wrap |
| `owner_key` (36) | KAT tegen OpenSSL (AES, P-256, SHA-256); in beide richtingen uitwisselbaar met de `openssl`-CLI; weigeren van SHA1, AES-128, weinig rondes, onversleuteld, P-384; mutatie-sweep |
| `setup_web_provision` (54) en `_provision_rx` (53), plus de OWNER-03-test in elke owner-build | nieuw apparaat tot geïnstalleerd: LAN geweigerd, STA aan geweigerd, stap 1 eenmalig, onderbroken wizard, WiFi-test, sleutel maken en controleren, oude sleutel en verkeerde zin geweigerd, herstel met blokkade, sleutel vervangen, releasesleutel dicht, captive redirect, geen resetroute |
| `ota_policy` (85) | onder andere v2-releases, downgrade, RF-test nooit in een release, pre-owner-image geweigerd |
| `web_auth` (95) | onder andere Codex SEC-02/03 (wrap) en het wissen van herstelcodes |
| `setup_web_provision_unreadable`, `_ambiguous` | onleesbaar wachtwoord in een onvoltooide installatie (OWNER-02); opgeslagen maar niet terug te lezen: stap 1 blijft dicht en oude codes zijn weg, ook bij een half mislukte registratie (OWNER-06/07) |
| CLI (43), pagina (136) | onder andere aanmelden voor OTA, releasebundel, de wizard in de pagina, de herstelpaden |

Ook gebouwd: alle 11 firmware-omgevingen.

Hardware: **NOT TESTED**. Zie het validatieplan.
