# Orcon Controller installeren

Hiermee bedien je je Orcon-ventilatie (HRC/MVS met een 15RF-afstandsbediening) vanaf je telefoon. Je hebt maar één keer een computer nodig: om de firmware op de Heltec te zetten. Daarna doe je alles met je telefoon, ook updates en herstel.

> Status: de firmware voor deze procedure is gebouwd en lokaal getest, maar nog niet op echte hardware gevalideerd. Er is ook nog geen gepubliceerde release. Deze handleiding beschrijft de procedure zoals die na de eerste release werkt.

## Wat je nodig hebt

- Een **Heltec WiFi LoRa 32 V2**, de 868 MHz-versie met antenne. Andere borden worden niet ondersteund.
- Eenmalig: een computer met Chrome of Edge, en een USB-kabel (micro-USB, geschikt voor data).
- Een telefoon met WiFi.
- Je WiFi-netwerk (2,4 GHz) en het wachtwoord daarvan.
- Voor stap 3 bij voorkeur je originele Orcon-afstandsbediening.

## 1. Firmware installeren (één keer, via USB)

1. Download van de releasepagina van dit project het bestand `orcon-controller-heltec-v2-factory.bin`.
2. Open in Chrome of Edge de officiële webflasher van Espressif: <https://espressif.github.io/esptool-js/>.
3. Sluit de Heltec aan. Kies **Connect** en selecteer de USB-poort.
4. Kies het bestand bij adres **0x0** en klik op **Program**.
5. Druk na afloop één keer kort op **RST**.

> Werkt de USB-poort niet? Gebruik een andere kabel (sommige kabels laden alleen). Op Windows heb je soms de CP210x-driver van Silicon Labs nodig.

## 2. Verbinden met de Heltec

1. Het scherm toont **Installatie**, de netwerknaam **Orcon15RF-Setup** en een wachtwoord van 12 tekens. Dat wachtwoord is uniek voor jouw apparaat.
2. Verbind je telefoon met **Orcon15RF-Setup**.
3. Meestal opent de installatiepagina vanzelf. Gebeurt dat niet, open dan in Safari of Chrome **http://192.168.4.1**.

Tip: Opent de pagina in een klein inlogvenster? Kies dan bij stap 4 **Tekst kopiëren**, of open http://192.168.4.1 in Safari of Chrome. Daarna ga je verder waar je was.

## 3. De installatiewizard (vier stappen)

**Stap 1 — Welkom bij Orcon Controller.** Kies een beheerderswachtwoord van minstens 8 tekens. Er is geen standaardwachtwoord.

**Stap 2 — Thuisnetwerk.**
1. Tik op **Scannen** en kies je netwerk, of typ de naam van een verborgen netwerk.
2. Vul het wachtwoord in en tik op **Verbinding testen**.
3. Het netwerk wordt pas opgeslagen als de test lukt.

Je telefoon kan tijdens de test even de verbinding met Orcon15RF-Setup verliezen. Maak dan opnieuw verbinding; de pagina gaat vanzelf verder.

**Stap 3 — Ventilatie.** Maak een profiel, bijvoorbeeld *Woonkamer* of *Zolder*.
- **Afstandsbediening uitlezen:** druk de knoppen in die de pagina aanwijst.
- **Koppelen met de ventilatie-unit:** zet de unit in koppelmodus. Haal hem minstens 10 seconden van de stroom en zet hem weer aan; de unit accepteert dan ongeveer 3 minuten een koppeling. Tik daarna op **Auto + 1** verzenden.

Lukt het nu niet? Kies **Later doen**; je kunt dit altijd later via *Profielen* doen.

**Stap 4 — Je persoonlijke herstelsleutel.**
1. Kies een wachtwoordzin van minstens 10 tekens. Neem een andere dan je beheerderswachtwoord.
2. Tik op **Sleutel maken**. Dat duurt ongeveer 10 seconden.
3. Tik op **Download** en bewaar `orcon-recovery-key.pem`, of kopieer de tekst.
4. Kies het opgeslagen bestand (of plak de tekst), vul de wachtwoordzin in en tik op **Controleren en afronden**.

Bewaar het bestand en de wachtwoordzin apart en veilig, bijvoorbeeld in je wachtwoordmanager. Raak je ze allebei kwijt én vergeet je je wachtwoord, dan rest alleen een fabrieksreset (profielen en koppeling gaan dan verloren).

**Klaar.**
1. De Heltec start opnieuw en verbindt met je thuisnetwerk.
2. Verbind je telefoon weer met je thuisnetwerk.
3. Open **http://orcon15rf.local**, of het IP-adres dat de pagina toont.
4. Meld je aan met je beheerderswachtwoord.

## Updates

1. Download het nieuwe updatebestand (`.orconfw`) van de releasepagina.
2. Open in de webinterface **Systeem → Firmware → Update installeren** en kies het bestand.

De Heltec controleert of de update officieel ondertekend is, en of hij niet ouder is dan de geïnstalleerde versie. Werkt de nieuwe firmware niet goed, dan keert hij binnen 5 minuten vanzelf terug naar de vorige versie.

## Herstellen met de knoppen

De Heltec heeft twee knoppen: **RST** (herstart) en **PRG**.

1. Druk kort op **RST** en laat los.
2. Houd daarna, binnen 20 seconden, **PRG** 15 seconden ingedrukt. Het scherm telt af.
3. Het herstelmenu verschijnt. Laat PRG los.
   - **Kort drukken** = volgende optie.
   - **2 seconden vasthouden** = kiezen.

| Optie | Wanneer | Wat blijft | Wat verdwijnt |
|---|---|---|---|
| **Annuleren** | per ongeluk | alles | niets |
| **Herstelmodus** | wachtwoord vergeten, of een nieuwe herstelsleutel maken | alles | niets |
| **WiFi wissen** | nieuwe router, verhuizing | wachtwoord, sleutel, profielen, koppeling | alleen het thuis-WiFi |
| **Fabrieksreset** | verkopen, weggeven, helemaal opnieuw | de firmware | alles: WiFi, wachtwoord, sleutel, profielen, koppeling |

**Herstelmodus**
1. Verbind je telefoon met Orcon15RF-Setup (wachtwoord op het scherm) en open http://192.168.4.1.
2. Kies **Wachtwoord vergeten** en geef je herstelsleutel en wachtwoordzin op. Stel daarna een nieuw wachtwoord in.
3. Weet je je wachtwoord nog? Meld je aan en maak via **Systeem → Herstelsleutel** een nieuwe sleutel. De oude werkt daarna niet meer.

De herstelmodus duurt 15 minuten.

**WiFi wissen**
1. Verbind met Orcon15RF-Setup.
2. Meld je aan en stel een nieuw netwerk in.

**Fabrieksreset**
1. Het scherm vraagt om bevestiging. Laat PRG los en houd hem daarna opnieuw **5 seconden** vast. Kort drukken annuleert.
2. De Heltec wist alles en start opnieuw.
3. Begin weer bij stap 2 van deze handleiding.

Een fabrieksreset maakt ook de herstelsleutel van de vorige eigenaar waardeloos.

## Problemen oplossen

| Probleem | Oplossing |
|---|---|
| Scherm blijft zwart na RST | Je hield PRG al vast tijdens de herstart. Laat alles los en druk alleen kort op RST. |
| Herstelmenu verschijnt niet | Begin met RST en houd PRG vast binnen 20 seconden na de herstart, en dan 15 seconden lang. |
| "Update nog niet bevestigd" in het menu | Na een update test de Heltec zichzelf maximaal 5 minuten. Probeer het daarna opnieuw. |
| Netwerk niet gevonden | Alleen 2,4 GHz-netwerken werken. Controleer de naam (hoofdletters tellen mee). |
| Download van de sleutel lukt niet | Gebruik **Tekst kopiëren**, of open http://192.168.4.1 in Safari of Chrome. |
| Wachtwoord vergeten, geen sleutel meer | Fabrieksreset en opnieuw installeren. |
| Scherm toont **Reset nodig (PRG)** | De Heltec is zijn instellingen kwijt (zeldzame opslagfout) maar had een eigenaar. Uit veiligheid start de installatie pas na een fabrieksreset met de knoppen. |
| Scherm toont **Opslag onleesbaar: RST, PRG 15 s vast = fabrieksreset** | Een deel van de opslag is beschadigd. Uit veiligheid start de Heltec niet normaal. Doe de fabrieksreset met de knoppen. Staat er "Reset mislukt", probeer het dan opnieuw. Blijft dat zo, dan is herstel via USB nodig. |
| Scherm toont **Opslag onleesbaar, update onbevestigd: RST = vorige versie** | De net geïnstalleerde update kan de opslag niet lezen. Druk kort op RST: de Heltec gaat terug naar de vorige versie. |

## Veiligheid in het kort

- Elk apparaat heeft zijn eigen setup-wachtwoord, beheerderswachtwoord en herstelsleutel. Er bestaat geen gedeeld fabriekswachtwoord.
- De installatie en het herstel werken alleen via het directe, beveiligde netwerk van de Heltec, nooit via je thuisnetwerk.
- Een fabrieksreset kan alleen met de knoppen, niet via het netwerk.
- Op je thuisnetwerk gebruikt de webinterface HTTP, zonder versleuteling. Gebruik hem op een vertrouwd netwerk. Achtergrond: [`PROVISIONING.md`](PROVISIONING.md).
