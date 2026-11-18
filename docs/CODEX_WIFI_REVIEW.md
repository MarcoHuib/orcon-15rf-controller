# Onafhankelijke review van de WiFi-reconnect (Codex) — 2026-11-17

**Wat is beoordeeld:** `feature/wifi-reconnect` ten opzichte van 1a92fb0 (de `src/` van 2edb423, Codex ronde 7 GO).
- Ronde 1: dbeed9e.

**Aanleiding:** de Heltec was onbereikbaar. Gemeten:
- het ARP-record op de Mac voor het oude adres bleef `incomplete`;
- `orcon15rf.local` werd niet gevonden;
- het OLED toonde nog het oude IP-adres.

Na een RST werkte alles weer, met een ander DHCP-adres. Er was geen crashdump.

**Diagnose: wat de meting wel en niet verklaart**
- **ARP `incomplete` is een fout op netwerkniveau.** Op het oude adres antwoordde niemand meer op ARP. Dat ligt onder HTTP. Een webserver die met 403 weigert, beantwoordt ARP gewoon, want dat doet de netwerkstack. **Een 403 kan de ARP-fout dus niet verklaren.**
- **Een ESP32 met verbinding stuurt elke 60 s een gratuitous ARP** (lwIP-instelling). Was het board op het oude adres verbonden geweest, dan was het `incomplete`-record vanzelf opgelost. Het board was op dat adres dus niet verbonden. Het was **offline**, of **online op een ander adres**.
- **Offline past bij alle drie de waarnemingen.** De Arduino-kern (2.0.16) probeert alleen opnieuw te verbinden bij redenen op zijn eigen lijst. Na de eerste mislukking stopt hij voorgoed bij elke andere reden, bijvoorbeeld 202 AUTH_FAIL terwijl een access point opstart.
  - Geen ARP en geen mDNS volgen dan vanzelf.
  - Het OLED toonde het oude adres, omdat `g_ip` een momentopname bij het opstarten was.
  - Na de RST gaf DHCP een ander adres, omdat het oude intussen was vrijgegeven.
  - **Dit is de meest waarschijnlijke verklaring.**
- **"Online op een ander adres" verklaart de ARP-fout op het oude adres ook.** Maar dan had `orcon15rf.local` naar het nieuwe adres moeten wijzen; voor de mDNS-fout is dan een extra storing nodig. **Minder waarschijnlijk.**
- **De 403 is een apart, echt defect.** Na een reconnect met een nieuw adres weigerde de Host-controle elk verzoek naar dat nieuwe adres, want `g_ip` was verouderd. Het board leek dan via het IP-adres kapot, terwijl het online was. Dat speelt alleen in het tweede scenario, en alleen voor wie het nieuwe adres probeerde. Het is dus geen verklaring voor de ARP-meting.
- **Achteraf is niet vast te stellen welk scenario het was.** De oude firmware had geen WiFi-diagnose en de RST wiste alles. De nieuwe `/api/diag` maakt het onderscheid de volgende keer wel:
  - `outages`, `last_outage_s` en `last_reason` laten zien dat het board offline was, hoe lang en waarom;
  - `ip_changes` laat zien dat het board online een ander adres kreeg. Een ARP-fout op het oude adres is dan te verwachten.

Beide defecten zijn hersteld: de kern-retry door de watch, en de verouderde `g_ip` door het live adres.

**Uitvoering:**
- Codex CLI 0.169 (VS Code-extensie), in een losse kloon van alleen deze branch, zonder remote.
- Niet in de kloon: `.flashtools`, captures en build-uitvoer. Alleen ArduinoJson is meegekopieerd, voor de hosttests.
- Niet gebruikt: hardware, netwerk, GitHub en GitHub Actions.

## Ronde 1 (dbeed9e): GO

| ID | Ernst | Bevinding |
|----|-------|-----------|
| WIFI-01 | MINOR | Is het board al aangemeld bij het access point en wacht het nog op een adres (DHCP), dan breekt elke nieuwe poging die aanvraag af. Een netwerk dat meer dan ongeveer 30 s nodig heeft voor DHCP, raakt dan pas na de herstart van 15 minuten weer verbonden. |
| WIFI-02 | MINOR | `g_staReason` wordt in de WiFi-eventtaak geschreven en in de looptaak gelezen. `volatile` voorkomt formeel geen data race. Alleen de diagnose is geraakt. |
| WIFI-03 | INFO | De RTC-markering bewijst niet sluitend dat de vorige herstart van de watch kwam. Een brown-out binnen de 100 ms na het zetten van de markering, of willekeurige RTC-inhoud na het inschakelen, kan een vals "ja" geven. Alleen de diagnose is geraakt. |
| WIFI-04 | INFO | Valt een verbinding stil weg terwijl de kern nog `WL_CONNECTED` meldt, dan start de watch niet. Of het oorspronkelijke incident zo verliep, valt met deze wijziging niet uit te sluiten. |

**Geen bevinding:**
- De Host-controle volgt nu het live adres. Ze laat nog steeds alleen het numerieke eigen adres en de vaste mDNS-naam toe. Een vreemde DHCP-server kan er geen willekeurige hostnaam mee binnensmokkelen.
- `/api/diag` toont geen geheimen.
- AP-modus, de WiFi-test van de wizard, herstelmodus, installatie, fabrieksreset, OTA-upload en -hold, MQTT, ArduinoOTA en het RF-budget worden niet geraakt.
- De OTA-vestigingsregel (OWNER-10) is juist toegepast: geen herstart tijdens een onbevestigde update.

**Tests van de reviewer:**
- Alle C++-hostsuites slagen, waaronder 17 WiFi-watch-checks.
- De paginacontroles slagen (137).
- `test_sx1276_tx.py` kon niet draaien, omdat in de kloon de RadioLib-header ontbrak. In de eigen worktree slaagt de volledige suite.

## Herstel
- **WIFI-01:** de watch krijgt "aangemeld bij het access point" als extra invoer (`esp_wifi_sta_get_ap_info() == ESP_OK`, dezelfde IDF-aanroep die `WiFi.RSSI()` gebruikt; de Arduino-statusbit bestaat niet in de nieuwere kern van de v3-builds). Zolang het board aangemeld is en op een adres wacht, krijgt DHCP 60 s voordat een nieuwe poging volgt. De herstart na 15 minuten blijft gelden. Hosttest: 25 checks.
- **WIFI-02:** `g_staReason` is nu `std::atomic<uint8_t>`, met relaxed load en store.
- **WIFI-03:** `watch_restart` vraagt nu ook om `esp_reset_reason() == ESP_RST_SW`. Het blijft een indicatie, geen bewijs.
- **WIFI-04:** gedocumenteerd als grens in `src/wifi_watch.h`. Er is bewust geen liveness-probe gebouwd: een ping naar de gateway ziet geen problemen met inkomende broadcasts, en het incident past beter bij de twee herstelde gaten. Komt het terug, dan laat `/api/diag` het zien: `"up":true` zonder nieuwe onderbreking. Dan volgt een apart ontworpen probe.

**Niet getest:** echte drivertiming, DHCP-gedrag van verschillende routers, verborgen SSID's, WPA3, de AP+STA-wizard met een telefoon, en het opstartvenster van 15 s. Hardware: NOT TESTED.

## Ronde 2 (43b6f3e): GO

| ID | Ernst | Bevinding |
|----|-------|-----------|
| WIFI-05 | MINOR | Na een nieuwe poging bleef `joined_` staan. Werd het board tussen twee metingen in weer aangemeld, dan kreeg die nieuwe aanmelding het verlopen DHCP-venster. De volgende poging kwam dan na 30 s en kon een DHCP-aanvraag van 40 tot 60 s afbreken. |
| WIFI-06 | MINOR (bestaand) | `g_wtReason` van de WiFi-test in de wizard had dezelfde race tussen de eventtaak en de looptaak als WIFI-02. |

Status van ronde 1: WIFI-01 PARTIAL (door WIFI-05), WIFI-02 FIXED, WIFI-03 FIXED, WIFI-04 DOCUMENTED.

**Wel gecontroleerd en in orde:**
- Het DHCP-venster kan de herstart na 15 minuten niet uitstellen. De herstartcontrole komt vóór het venster, en een nieuwe aanmelding zet de starttijd van de onderbreking niet terug.
- AP-modus, de WiFi-test van de wizard, OTA, fabrieksreset, de Host-controle en `/api/diag`: geen regressie gevonden.

## Herstel na ronde 2
- **WIFI-05:** een nieuwe poging wist `joined_`. Elke volgende aanmelding krijgt zo een vers venster van 60 s, ook als de tussenliggende "niet aangemeld"-toestand niet gemeten werd. De nieuwe hosttest faalt zonder deze regel (2 fouten) en slaagt ermee (28 checks).
- **WIFI-06:** `g_wtReason` is nu ook `std::atomic<uint8_t>`, met relaxed load en store.

## Aanvulling na ronde 2 (eigenaarsvragen, 18-11)
- **De herstart onderbreekt niets kritieks.** `mayRestartNow()` in `src/net.cpp` staat de herstart na 15 minuten alleen toe als er niets loopt:
  - geen onbevestigde update (OWNER-10);
  - het PRG-menu is niet open;
  - er is geen RF-zending bezig;
  - er loopt geen lees- of koppelsessie (`setupweb::busy()`).

  Een OTA-upload loopt helemaal binnen `server.handleClient()`, in dezelfde looptaak als de watch, dus de watch draait nooit midden in een upload. Een nieuwe poging gebeurt alleen zonder verbinding en raakt alleen WiFi, niet de 868-MHz-radio.
- **Diagnose:** `/api/diag` → `wifi` heeft twee nieuwe velden, `last_outage_s` (duur van de laatste onderbreking) en `ip_changes` (adreswissels terwijl het board online was). Daarmee is "offline" achteraf te onderscheiden van "online op een ander adres". Zie de aangepaste diagnose bovenaan.
- Deze aanvulling is beoordeeld in de Siri-review, ronde 3 ([`CODEX_SIRI_REVIEW.md`](CODEX_SIRI_REVIEW.md)).
