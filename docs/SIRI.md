# Siri: de ventilatie bedienen met je stem

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
1. Open de webinterface op je iPhone, meld je aan en ga naar **Systeem → Integraties → Siri**. Of tik op het dashboard op de kaart **Siri**.
2. Tik op **Siri aanzetten**. De Heltec maakt een eigen Siri-sleutel en toont vijf opdrachten.
3. Maak in de app **Opdrachten** een nieuwe opdracht (**+**):
   1. **Voeg actie toe** → **Haal inhoud van URL op**.
   2. Tik in de webinterface op **Kopieer** bij de stand en plak het adres in de actie.
   3. Tik op het pijltje in de actie en zet **Methode** op **POST**.
   4. Geef de opdracht de naam uit de lijst, bijvoorbeeld **Ventilatie stand 1**. Die naam zeg je tegen Siri.
4. Herhaal stap 3 voor de andere vier standen.

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

**Nieuwe sleutel** maakt de oude meteen ongeldig. Plak daarna de nieuwe adressen in je opdrachten. **Siri uitzetten** wist de sleutel. Een fabrieksreset wist hem ook.

## Hoe het werkt
`POST http://orcon15rf.local/api/siri?stand=<1|2|3|auto|afwezig>&k=<Siri-sleutel>`

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
- **Zonder HTTPS** gaat de sleutel onversleuteld over je eigen WiFi. Wie je WiFi-wachtwoord heeft en het verkeer afluistert, kan hem lezen, en daarmee alleen de stand kiezen, binnen het RF-budget. Vermoed je dat? Tik op **Nieuwe sleutel**. HTTPS blijft een apart punt ([`HTTPS_ONTWERP.md`](HTTPS_ONTWERP.md)).
- **Wachtwoord vergeten of gewijzigd:** de Siri-sleutel blijft geldig. Vervang hem zelf als je denkt dat iemand anders je wachtwoord had.

## Waarom Opdrachten en (nog) niet HomeKit
- **HomeKit** kent een ventilator als 0–100 % plus automatisch/handmatig. "Afwezig" en jouw vaste zinnen passen daar alleen omslachtig in, via vijf scènes.
- HomeKit vraagt een grote extra bibliotheek op de ESP32, een eigen koppelcode en sleutelopslag, en dus een grote nieuwe security-review van koppelen, reset en eigendom.
- **Opdrachten** gebruikt precies jouw zinnen en de bestaande controles. Er is maar één kleine, beperkte route bij gekomen.
- HomeKit blijft mogelijk als latere optie, bijvoorbeeld voor de Woning-app of bediening buitenshuis via een woninghub.

Review: [`CODEX_SIRI_REVIEW.md`](CODEX_SIRI_REVIEW.md).

## Niet getest
De exacte iOS-menunamen per iOS-versie, hoe Siri het antwoord voorleest, en het geheel op de echte Heltec. Dat volgt bij de eerste installatie.
