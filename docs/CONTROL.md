# Productie-image voor dagelijks gebruik (`heltec_v2_sx1276_control`)

Dit image bedient de Orcon via de webinterface. Je hebt er geen USB-kabel en geen originele afstandsbediening voor nodig.

- **Bediening:** de vijf standen, plus koppelen met de eigen Heltec-ID.
- **Begrenzing:** de RF-zendingen zijn begrensd door het doorlopende RF-budget. De testlimiet per opstart geldt hier niet.
- **Toegang:** alleen na aanmelden.
- **OTA:** het image is de ondertekende OTA-variant `control`. Het RX-only-image (`heltec_v2_sx1276`) blijft het rollbackdoel.

## RF-budget (`src/tx_budget.*`)

**Limieten**
- Maximaal **10 pakketten per doorlopende minuut** en **60 per doorlopend uur**.
- Daarnaast maximaal **3,6 s gemeten zendtijd per uur**: 0,1 % duty cycle. De wettelijke grens in 868,0–868,6 MHz is 1 %.
  - Een standcommando duurt ongeveer 11 ms, een koppelaanbod ongeveer 22 ms.
- Deze regels blijven gewoon gelden:
  - minimaal 2 s tussen commando's;
  - één transactie tegelijk;
  - geen automatische herhalingen.
- Koppelen start alleen als er ruimte is voor zowel het aanbod als de afsluiting.

**Afdwinging**
- In de RF-backend, in de RX-taak, vlak voordat de radio wordt aangesproken.
- Elke start telt mee, ook een mislukte: eerst met de geschatte zendtijd, daarna met de gemeten zendtijd.
  - `txBegin` meet die tijd van de TX-start tot `PacketSent`.

**Bewaring**
- De geschiedenis staat in RTC-geheugen, met een CRC, en wordt getimed met de RTC-timer.
- Een softwareherstart, crash, watchdog of OTA-herstart wist hem dus niet. Een stroomonderbreking, die alleen fysiek kan, wist hem wel.
- Er wordt niets naar NVS geschreven.

**Zichtbaarheid**
- **Webinterface:** toont het resterende budget. Bij de limiet verschijnt: "Zendlimiet bereikt … Er is niets verzonden. Opnieuw mogelijk om HH:MM:SS".
- **API:** `429 {"error":"tx-budget","retry_s":N}`.
- **Console:** per TX een regel `TX #n zendtijd x ms; budget a/10 per minuut, b/60 per uur`.

## Aanmelden (`src/web_auth.*`, `src/auth_store.cpp`)

Er is **geen standaardwachtwoord**.

**Eerste wachtwoord, of wachtwoord vergeten**
1. Vraag op de Mac met `~/.orcon/ota-signing-key.pem` een herstelcode aan: `python3 tools/orcon.py auth-code`.
   - De Heltec geeft de code alleen voor een verzoek dat met de OTA-sleutel is ondertekend.
   - Het verzoek is gebonden aan een eenmalige nonce en aan dit board.
2. Kies in de webinterface "Wachtwoord vergeten" (of het instelscherm) en vul de code en een nieuw wachtwoord in (8 tot 64 tekens).
3. De code is 10 minuten geldig, werkt één keer en staat 5 foute pogingen toe.

**Opslag**
- PBKDF2-HMAC-SHA256 met 10 000 rondes en een willekeurige salt, in NVS (`orconauth/cred`).
- Een zelftest met een bekende uitkomst draait eerst. Mislukt die, dan blijft aanmelden dicht.

**Sessies**
- In RAM, maximaal 4.
- Cookie `orcon_sid` met `HttpOnly` en `SameSite=Strict`.
- Bij elke wijziging is het CSRF-token van de sessie verplicht.
- Een sessie verloopt na 7 dagen ongebruikt, of uiterlijk na 30 dagen. Na een herstart meld je opnieuw aan.

**Pogingen**
- 4 mislukte pogingen zijn vrij. Daarna volgt een wachttijd van 30 s, die verdubbelt tot 15 min.
- Ook het juiste wachtwoord moet die wachttijd afwachten.

**Afmelden van andere apparaten**
- Na een wachtwoordwijziging (Systeem → Account) worden alle andere sessies afgemeld.
- Na het instellen met een herstelcode ook.

**Wat beschermd is**
- **Achter de login:** bediening, profielen, uitlezen, koppelen, en WiFi, OLED, herstarten en "WiFi vergeten".
- **Open, alleen lezen:** console (ID's gemaskeerd), capture, diag, OTA-status, `/api/state` en `/api/log`. Daardoor werkt `orcon.py logs` zonder wachtwoord.
- **OTA:** ongewijzigd, beveiligd met een eigen handtekening.
- **Logs:** nooit wachtwoorden of codes, ook niet in de console.

**Beperking**
- HTTP op het thuisnetwerk, zonder TLS. Wie het WiFi-verkeer kan afluisteren, kan een sessiecookie overnemen.

## Nood-WiFi (accesspoint `Orcon15RF-Setup`)

**Wachtwoord**
- Willekeurig per apparaat (10 tekens), één keer opgeslagen.
- Het staat alleen op het OLED, zolang het accesspoint aan staat. Het scherm blijft dan aan.
- Het adres is `http://192.168.4.1`.
- `orconsetup` geldt alleen nog voor het 4848-board zonder OLED.

**Wat kan in deze modus**
- Aanmelden en WiFi instellen.
- RF-bediening alleen via het thuisnetwerk.

**Automatisch herstel**
- Was het thuisnetwerk bij het opstarten niet binnen 15 s bereikbaar (bijvoorbeeld een router die trager opstart na een stroomstoring)?
- Dan probeert de Heltec het elke minuut opnieuw, behalve als er iemand met het accesspoint verbonden is.
- Zodra het netwerk er is, schakelt hij het accesspoint uit.

## RX-only-rollbackimage

- Ook met aanmelden: hetzelfde wachtwoord, uit dezelfde NVS.
- `/api/ids` (identiteit wijzigen) en `/api/cmd` zijn verwijderd.
- `/api/wifi`, `/api/oled`, `/api/netcfg` en `/api/scan` werken alleen na aanmelden.
- Het klassieke dashboard blijft leesbaar, maar kan niets meer wijzigen.

## Installeren (alleen na expliciete toestemming per OTA)

Gebruik twee ondertekende OTA's, zodat het RX-only-image het rollbackdoel blijft. OTA schrijft altijd naar de sleuf die niet draait.

1. **RX-only met login:**
   `python3 tools/orcon.py ota-install --worktree <worktree> --env heltec_v2_sx1276 --yes`
2. **Control:**
   `python3 tools/orcon.py ota-install --worktree <worktree> --env heltec_v2_sx1276_control --yes --tx-ack "control firmware"`
3. **Wachtwoord instellen:** `python3 tools/orcon.py auth-code`, daarna in de webinterface.

**Lokaal verpakken**
- `tools/heltec_v2.sh package|verify` accepteert de control-env alleen met `ORCON_CONTROL_ACK="control firmware"`.
- Een USB-flash vraagt de bevestigingszin nog een keer.
