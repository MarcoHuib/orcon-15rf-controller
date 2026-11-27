# Siri: de ventilatie bedienen met je stem

Deze integratie werkt via Apple's app **Opdrachten** (Shortcuts). Het is **geen rechtstreekse Apple Woning-/HomeKit-integratie**. Een eigen sleutel geeft alleen toegang tot de vijf ventilatiestanden op je vertrouwde thuisnetwerk.

Zeg tegen je iPhone:
- "Hé Siri, ventilatie stand 1", "stand 2" of "stand 3";
- "Hé Siri, ventilatie automatisch";
- "Hé Siri, ventilatie afwezig".

Je iPhone stuurt het commando via je thuisnetwerk rechtstreeks naar de Heltec. Er is geen server, cloud of abonnement nodig, en er hoeft geen poort in je router open.

## Wat je nodig hebt
- De control-firmware (die kan zenden). Op RX-only-firmware is de Siri-kaart uitgeschakeld.
- Een gekoppelde unit en een actief profiel, net als voor de bedieningsknoppen.
- Een iPhone met de app **Opdrachten**, verbonden met je thuisnetwerk.

## Instellen (eenmalig, op je iPhone)
1. Open de webinterface op je iPhone, meld je aan en tik op het dashboard op de kaart **Siri** (dat opent Integraties → Siri).
2. Tik op **Siri aanzetten**. De Heltec maakt een eigen Siri-sleutel en toont vijf opdrachten.
3. Maak in de app **Opdrachten** een nieuwe opdracht (**+**):
   1. **Voeg actie toe** → **Haal inhoud van URL op**.
   2. Tik in de webinterface op **Kopieer** bij de stand en plak het adres in de actie. Het adres bevat alleen de stand, bijvoorbeeld `http://orcon15rf.local/api/siri?stand=1`.
   3. Tik op het pijltje in de actie en zet **Methode** op **POST**.
   4. Tik bij **Headers** op **Voeg nieuwe header toe**. Typ als sleutel `Authorization`. Tik in de webinterface op **Sleutel kopiëren** en plak dat als waarde. Dat is `Bearer `, gevolgd door je sleutel.
   5. Geef de opdracht de naam uit de lijst, bijvoorbeeld **Ventilatie stand 1**. Die naam zeg je tegen Siri.
4. Kies daarna bij die opdracht **Dupliceer** en pas alleen het adres en de naam aan. Doe dat voor de andere vier standen.

De eerste keer vraagt iOS of Opdrachten apparaten in je lokale netwerk mag zoeken. Kies **Sta toe**.

Siri antwoordt met een korte zin van de Heltec:

| Situatie | Antwoord |
|---|---|
| Verstuurd | "De ventilatie gaat naar stand 2." |
| Stond al zo | "De ventilatie staat al op stand 2." |
| RF-budget op | "Even wachten: de zendlimiet is bereikt. Probeer het over N seconden opnieuw." |
| Afstandsbediening net gebruikt, of een eerder commando loopt nog | "De ventilatie is even bezig. Probeer het zo opnieuw." |
| Geen profiel actief | "Kies eerst een profiel in de webinterface." |
| Verkeerde of oude sleutel, of Siri uit | "De Siri-sleutel klopt niet. …" |
| Niet vanaf je thuisnetwerk | "Siri werkt alleen vanaf je eigen thuisnetwerk." |

**Nieuwe sleutel** maakt de oude meteen ongeldig. Plak daarna de nieuwe sleutel in de header van elke opdracht. **Siri uitzetten** wist de sleutel. Een fabrieksreset wist hem ook.

## Hoe het werkt
```
POST http://orcon15rf.local/api/siri?stand=<1|2|3|auto|afwezig>
Authorization: Bearer <Siri-sleutel>
```

- **Zelfde zendroute als de webknoppen** (`ctlAdmit` in `src/setup_web.cpp`): gekoppelde unit, actief profiel, identiteitscontrole, het RF-budget (10 per minuut, 60 per uur, 3,6 s zendtijd per uur), minimaal 2 s tussen commando's, niet tijdens uitlezen of koppelen, en de wachttijd na een druk op de afstandsbediening.
- **Alleen de vijf standen** (`afwezig` mag ook `away` heten). De timer kan niet via Siri.
- **Alleen in normaal gebruik:** niet tijdens de installatie, en niet als de Heltec na opslagverlies op slot staat (Locked).
- Het antwoord is platte tekst in het Nederlands.

## Veiligheid
- **De Siri-sleutel is geen login.** Hij kan alleen de vijf standen kiezen: geen instellingen, geen wachtwoord, geen firmware, geen koppelen. Je beheerderswachtwoord staat nergens in een opdracht.
- **De sleutel:** 128 bits uit de hardware-RNG van de ESP32, als 32 hexadecimale tekens. Hij staat in NVS (`orconauth/siri`) naast het wachtwoordrecord en wordt in constante tijd vergeleken. Alleen de aangemelde eigenaar ziet hem in de webinterface; zo kun je later nog een iPhone instellen. Is de opslag onleesbaar, dan weigert Siri, en meldt de webinterface een opslagfout in plaats van "uit". Uitzetten telt pas als de sleutel aantoonbaar weg is. Het maken of wissen komt niet in de (openbare) console.
- **Alleen vanaf je thuisnetwerk:**
  - De client moet in hetzelfde subnet zitten als de Heltec (socketadressen, geen headers). Een doorgestuurd verzoek van internet of uit een ander subnet houdt zijn vreemde bronadres en wordt geweigerd. Een router die bij port-forwarding ook het bronadres vervangt, wordt hiermee niet herkend; zet dus nooit een poort open.
  - Nooit via het setup-accesspoint.
  - De bestaande Host-controle tegen DNS-rebinding geldt ook hier.
- **Alleen POST.** Een linkvoorbeeld in Berichten of een prefetch (GET) zet de ventilatie nooit om.
- **Nooit in een URL.** De sleutel gaat alleen in de header `Authorization`. Een sleutel in de URL (`?k=…`) wordt genegeerd en het verzoek geweigerd. De adressen in de webinterface bevatten alleen de stand.
  - Een URL komt op meer plekken terecht dan een header: in logs van servers en proxy's, in linkvoorbeelden, in de browsergeschiedenis als je hem plakt.
  - **Een gedeelde opdracht bevat ook de header**, dus ook de sleutel. Deel je Siri-opdrachten niet.
- **Niet in logs bij normaal gebruik.**
  - De firmware schrijft de sleutel nergens weg: niet naar de console, niet naar de seriële poort, niet naar NVS buiten `orconauth/siri`.
  - De WebServer van de Arduino-kern logt de verzoekregel, headers en body alleen op het niveau "verbose". De firmware bouwt met `CORE_DEBUG_LEVEL=2`, dus die code zit er niet in.
  - Uitzondering: bij een kapot verzoek (een onleesbare verzoekregel of een kapotte multipart-body) schrijft de WebServer die regel of dat stuk body als fout naar de seriële poort. Daar staat de sleutel bij een gewoon Siri-verzoek niet in, want hij zit in een header. De seriële poort is alleen via USB aan het board te lezen.
  - Het maken of wissen van de sleutel komt niet in de console, want die is zonder aanmelden leesbaar.
- **Wachtwoord vergeten of gewijzigd:** de Siri-sleutel blijft geldig. Vervang hem zelf als je denkt dat iemand anders je wachtwoord had.

## Het resterende risico: HTTP zonder versleuteling
Ook de gepubliceerde firmware spreekt op het thuisnetwerk nog geen HTTPS ([`HTTPS_ONTWERP.md`](HTTPS_ONTWERP.md)). Elk Siri-verzoek gaat daarom **zonder TLS-versleuteling** over je eigen netwerk, inclusief de header met de sleutel. Dit blijft een relevante beveiligingsbeperking, ondanks de beschikbare officiële releases.

**Wie de sleutel kan zien:**
- iemand met je WiFi-wachtwoord die het verkeer opneemt. Bij WPA2-Personal moet diegene ook de aanmelding van je iPhone opvangen (of afdwingen door de verbinding te verbreken); daarna is het verkeer te ontcijferen.
- een besmet of onbetrouwbaar apparaat in je netwerk, bijvoorbeeld via ARP-spoofing;
- je router;
- iedereen met wie je een Siri-opdracht deelt (zie hierboven).

**Wat iemand met alleen de Siri-sleutel kan:** de stand kiezen. Steeds binnen het RF-budget (10 per minuut, 60 per uur), alleen vanaf je eigen thuisnetwerk, en alleen zolang de sleutel geldig is.

**Wat diegene er niet mee kan:**
- instellingen wijzigen of je wachtwoord zien;
- firmware installeren, koppelen of de timer gebruiken;
- iets vanaf internet. Voorwaarde: er staat geen poort open, en er staat geen proxy of router tussen die het bronadres vervangt door een adres uit je thuisnetwerk (zie "Alleen vanaf je thuisnetwerk").

**Een actieve aanvaller in je netwerk is een ander verhaal.** Wie al tussen je apparaten en de Heltec zit (bijvoorbeeld met ARP-spoofing), kan ook het beheerwachtwoord en de sessiecookie van de webinterface onderscheppen of verzoeken veranderen. Dat geldt ook zonder Siri, zie SEC-04 in [`CODEX_PR10_SECURITY_REVIEW.md`](CODEX_PR10_SECURITY_REVIEW.md). Siri voegt geen nieuw soort blootstelling toe: de sleutel reist over dezelfde onversleutelde weg, maar geeft veel minder rechten. Pas HTTPS sluit dit af.

**Wat je kunt doen:**
- Vermoed je dat iemand meekeek, of heb je een opdracht gedeeld, tik dan op **Nieuwe sleutel**. De oude werkt meteen niet meer; zet de nieuwe in de header van je opdrachten.
- Gebruik voor gasten een gastnetwerk (een ander subnet), of geef ze je WiFi-wachtwoord niet.
- Zet nooit een poort naar de Heltec open.

## Waarom Opdrachten en (nog) niet HomeKit
- **HomeKit** kent een ventilator als 0–100 % plus automatisch/handmatig. "Afwezig" en jouw vaste zinnen passen daar alleen omslachtig in, via vijf scènes.
- HomeKit vraagt een grote extra bibliotheek op de ESP32, een eigen koppelcode en sleutelopslag, en dus een grote nieuwe security-review van koppelen, reset en eigendom.
- **Opdrachten** gebruikt precies jouw zinnen en de bestaande controles. Er is maar één kleine, beperkte route bij gekomen.
- HomeKit blijft mogelijk als latere optie, bijvoorbeeld voor de Woning-app of bediening buitenshuis via een woninghub.

Review: [`CODEX_SIRI_REVIEW.md`](CODEX_SIRI_REVIEW.md).

## Niet getest
De exacte iOS-menunamen per iOS-versie, hoe Siri het antwoord voorleest, en het geheel op de echte Heltec. Dat volgt bij de eerste installatie.
