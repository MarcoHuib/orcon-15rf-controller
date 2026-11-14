# Veilige webverbinding op het thuisnetwerk (onderzoek en ontwerp)

> Status: onderzoek, ontwerp en een bouwproef. Niet gebouwd in de firmware.
>
> Uitgangspunt van de maintainer: een wachtwoord dat via HTTP over het thuisnetwerk gaat, is niet voldoende beveiligd voor een definitieve release. Daarmee is dit een **releaseblokkade** (zie `RELEASES.md`).

## Het probleem

Op het thuisnetwerk gaan de pagina, het aanmelden (wachtwoord) en het sessiecookie over HTTP.

| Aanvaller | Wat die kan |
|---|---|
| **Passief** | Iedereen met het WiFi-wachtwoord en een opgenomen handshake van je telefoon kan WPA2-PSK-verkeer ontsleutelen: een huisgenoot, een gehackt IoT-apparaat, een buur die het wachtwoord kent. Diegene leest wachtwoord en cookie mee. |
| **Actief (MITM)** | Elk apparaat in het LAN kan met ARP-spoofing verkeer omleiden en de pagina zelf wijzigen. Dan helpt zelfs JavaScript-cryptografie niet: de aanvaller vervangt die code. |

**Wat een aanvaller met het wachtwoord kan:** de ventilatie bedienen, WiFi wijzigen, en een officiële (ondertekende) update installeren.

**Wat hij niet kan, door het eigendomsmodel:**
- de eigenaarssleutel vervangen (daar is fysieke aanwezigheid voor nodig);
- de eigenaar blijvend buitensluiten (herstel met het sleutelbestand in de herstelmodus);
- een fabrieksreset uitlokken.

## Opties

| Optie | Tegen passief | Tegen actief | Zonder cloud | Bruikbaar | Oordeel |
|---|---|---|---|---|---|
| 1. Publiek certificaat per apparaat (naam onder een domein van de uitgever, plus ACME DNS-01) | ja | ja | **nee**: DNS- en uitgiftedienst van de uitgever nodig | ja | afgewezen: cloudafhankelijkheid, privacy, beschikbaarheid |
| 2. Zelfondertekend certificaat (TOFU) | ja | **nee**: een aanvaller toont zijn eigen zelfondertekende certificaat en de gebruiker ziet geen verschil | ja | nee: volledige waarschuwing bij elke sessie | afgewezen: leert gebruikers waarschuwingen wegklikken |
| 3. **Eigen CA per apparaat, eenmalig per telefoon geïnstalleerd** | ja | ja | ja | redelijk: 2–4 stappen per telefoon | **aanbevolen** |
| 4. PAKE (SRP/SPAKE2) in JavaScript over HTTP | ja | **nee**: de code komt zelf over HTTP en wordt vervangen | ja | ja, maar een extra JS-cryptobibliotheek | onvoldoende voor de eis |
| 5. Aanmelden alleen via het setup-AP | ja | ja | ja | **nee**: geen bediening meer vanuit het thuisnetwerk | onbruikbaar |
| 6. Telefoon "koppelen" met een token, verder HTTP | gedeeltelijk | nee: token afluisterbaar en herspeelbaar | ja | ja | onvoldoende |

## Aanbeveling: eigen CA per apparaat (optie 3)

**Uitgangspunt**
- Tijdens de installatie, dus via het WPA2-setup-AP en met fysieke aanwezigheid, maakt het apparaat een eigen CA (P-256).
- De CA is beperkt met *name constraints* tot `orcon15rf.local` en het eigen adres.
- Daarmee tekent het apparaat zijn servercertificaat.
- De eigenaar installeert het CA-certificaat eenmalig op elke telefoon. Daarna werkt `https://orcon15rf.local` zonder waarschuwing en is het verkeer beschermd tegen zowel passieve als actieve aanvallers.

**Fasering**

| Fase | Inhoud |
|---|---|
| A. Firmware | `esp_https_server` (IDF httpd + mbedTLS) op poort 443, naast het bestaande HTTP. CA-sleutel en servercertificaat ontstaan op het apparaat en staan in NVS, net als de publieke eigenaarssleutel. Bij een nieuw IP-adres maakt het apparaat zelf een nieuw servercertificaat. |
| B. Wizard | Extra stap "Beveiligde verbinding": CA-profiel downloaden via het setup-AP, met uitleg per OS. Op iOS: profiel installeren, daarna *Instellingen → Algemeen → Info → Certificaatvertrouwen*. Op Android: *Beveiliging → Versleuteling en inloggegevens → CA-certificaat installeren*. Daarna wordt `https://orcon15rf.local` getest. |
| C. Beleid | HTTP op het LAN geeft alleen nog een redirect naar HTTPS en status zonder geheimen. Aanmelden en alle wijzigingen gaan alleen via HTTPS of via het setup-AP. Wie fase B overslaat, meldt zich aan via het setup-AP. Dat is een productkeuze. |
| D. CLI | `orcon.py` pint de CA van het apparaat (in `~/.orcon`). |

**Omvang**
- De webroutes zijn nu geschreven tegen de Arduino-`WebServer`. Voor `esp_https_server` is een adapterlaag nodig (httpd → het bestaande handlerpatroon), of een port van de routes.
- Inschatting: meerdere dagen werk, plus hardwarevalidatie.
- Een TLS-bibliotheek in Arduino-stijl (bijvoorbeeld `esp32_https_server`) wordt sinds 2021 niet meer onderhouden. Afgeraden.

## Gemeten (bouwproef, niet gecommit)

Arduino-ESP32 2.0.16 / IDF 4.4, Heltec V2, een minimale sketch met en zonder `esp_https_server`, en een wegwerpcertificaat:

| | Flash | Statisch RAM |
|---|---|---|
| Basis (WebServer) | 741 973 B | 44 680 B |
| Met `esp_https_server` | 877 821 B | 45 292 B |
| **Verschil** | **+135 848 B** | **+612 B** |

**NOT TESTED**
- Heap per TLS-sessie. Waarschijnlijk enkele tientallen KB; daarom maximaal 1–2 gelijktijdige TLS-sessies. De laagste vrije heap is nu ongeveer 160 KB.
- Duur van de handshake (P-256 met hardware-MPI).
- Hoe iOS, Android en Chrome een door de gebruiker geïnstalleerde CA met name constraints behandelen.

## Resterend risico, ook met optie 3

- **Fysieke aanvaller:** leest de CA-sleutel uit NVS (geen flash-encryptie) en kan zich dan tegenover de telefoons van dit huishouden voordoen als dit apparaat, alleen voor `orcon15rf.local`.
- **Een telefoon zonder geïnstalleerde CA:** volgt het beleid van fase C.
- **mDNS:** werkt slecht op sommige Android-versies. Dan het IP-adres gebruiken; het certificaat bevat het IP en wordt bij een nieuw adres opnieuw uitgegeven.
- **OWNER-05:** het bufferen van POST-bodies (DoS) blijft. TLS lost dat niet op.

## Tot die tijd (huidige stand)

- **Geheimen reizen alleen over het WPA2-setup-AP**, nooit over het LAN: het eerste wachtwoord, het sleutelbestand, het herstel.
- **Aanmelden op het LAN gaat over HTTP**, dus is het zichtbaar voor aanvallers in het LAN.
- **Beperkingen:**
  - een sterk WPA2/3-thuisnetwerk;
  - IoT-apparaten op een gastnetwerk;
  - sessies verlopen (7 dagen ongebruikt, 30 dagen maximaal);
  - een wachtwoordwijziging meldt alle sessies af;
  - herstel en sleutelwissel vragen fysieke aanwezigheid.
- **Oordeel:** voldoende voor een testfase, **niet voor de definitieve release** (uitgangspunt van de maintainer).
