# Onafhankelijke review van de WiFi-reconnect (Codex) — 2026-11-17

**Wat is beoordeeld:** `feature/wifi-reconnect` ten opzichte van 1a92fb0 (de `src/` van 2edb423, Codex ronde 7 GO).
- Ronde 1: dbeed9e.

**Aanleiding:** de Heltec was onbereikbaar. Gemeten:
- het ARP-record op de Mac voor het oude adres bleef `incomplete`;
- `orcon15rf.local` werd niet gevonden;
- het OLED toonde nog het oude IP-adres.

Na een RST werkte alles weer, met een ander DHCP-adres. Er was geen crashdump.

**Diagnose: wat de meting wel en niet verklaart.** Alles hieronder zijn hypotheses. Bewezen is alleen de eerste regel.
- **ARP `incomplete` is een fout op netwerkniveau.** Op het oude adres antwoordde niemand op ARP, gezien vanaf de Mac. Dat ligt onder HTTP. Een webserver die met 403 weigert, beantwoordt ARP gewoon, want dat doet de netwerkstack. **Een 403 kan de ARP-fout dus niet verklaren.**
- **Mogelijke verklaringen voor de ARP-fout:**
  1. **Offline.** De Arduino-kern (2.0.16) probeert alleen opnieuw te verbinden bij redenen op zijn eigen lijst. Na de eerste mislukking stopt hij voorgoed bij elke andere reden, bijvoorbeeld 202 AUTH_FAIL terwijl een access point opstart.
     - Verklaart alle drie de waarnemingen: geen ARP, geen mDNS, en het oude adres op het OLED (`g_ip` was een momentopname).
     - Dat DHCP na de RST een ander adres gaf, past hierbij, maar bewijst het niet.
     - **Waarschijnlijkst, met de gegevens die er zijn.**
  2. **Online op een ander adres.** Dit verklaart de ARP-fout op het oude adres ook. Maar `orcon15rf.local` had dan naar het nieuwe adres moeten wijzen, dus de mDNS-fout vraagt een extra storing.
  3. **Een stil gebroken verbinding op hetzelfde adres.** Het board meldt zich nog verbonden, maar verkeer komt niet aan: het access point stuurt niet door, clientisolatie, pakketverlies, of een vastgelopen link. Een ESP32 stuurt elke 60 s een gratuitous ARP, maar of die aankomt is niet gegarandeerd. Dit verklaart ARP en mDNS allebei. De watch ziet dit geval **niet** (grens, WIFI-04).
- **De 403 is een apart, echt defect.** Na een reconnect met een nieuw adres weigerde de Host-controle elk verzoek naar dat nieuwe adres, want `g_ip` was verouderd. Het board leek dan via het IP-adres kapot, terwijl het online was. Dat speelt alleen in verklaring 2, en alleen voor wie het nieuwe adres probeerde. Het verklaart de ARP-meting niet.
- **Achteraf is niet vast te stellen welke verklaring het was.** De oude firmware had geen WiFi-diagnose en de RST wiste alles. De nieuwe `/api/diag` → `wifi` levert de volgende keer **aanwijzingen, geen sluitend bewijs**:
  - `outages`, `last_outage_s`, `last_reason` en `retries`: het board zag zelf een onderbreking (verklaring 1), hoe lang en waarom.
  - `ip_changes`: het board kreeg online een ander adres (verklaring 2). Dit telt alleen wissels die de watch ziet terwijl het board online is, ook na een onderbreking. Wissels bij het opstarten en bij de overgang van het setup-AP naar het thuisnetwerk telt het niet mee, en het oude adres en het tijdstip worden niet bewaard.
  - `"up": true` zonder nieuwe onderbreking, terwijl het board onbereikbaar is: dat wijst op verklaring 3.
  - Alle tellers beginnen bij elke herstart opnieuw, ook na de herstart door de watch. Alleen `watch_restart` en de laatste reden (`last_reason`) gaan mee over die herstart, via RTC-geheugen.

Beide defecten zijn hersteld: de kern-retry door de watch, en de verouderde `g_ip` door het live adres. Verklaring 3 blijft een open grens.

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
- **De herstart onderbreekt niets kritieks.** `restartBlock()` in `src/net.cpp` noemt wat de herstart na 15 minuten tegenhoudt. Er volgt pas een herstart als niets van dit loopt:
  - een onbevestigde update (`update`, OWNER-10);
  - het PRG-menu (`menu`);
  - koppelen via `/api/cmd` of MQTT (`pairing`);
  - een RF-zending of een lees- of koppelsessie via de webinterface (`rf-or-session`, `setupweb::busy()`).
- **Een net geleerde koppeling gaat niet verloren.** De watch draait in `loop()` pas na het opslaan daarvan.
- **OTA:** een upload loopt helemaal binnen `server.handleClient()`, in dezelfde looptaak als de watch. De watch draait dus nooit midden in een upload. Wat wel kan: een sessie die klaarstaat maar nog niet uploadt (`Ready`), gaat bij een herstart verloren. Dan begint de CLI opnieuw.
- **Nieuwe pogingen:** alleen zonder verbinding, en alleen WiFi; de 868-MHz-radio wordt niet aangeraakt. Vlak vóór het verbreken wordt de status nog eens gelezen, zodat een net herstelde verbinding blijft staan.
- **Geen timeout die dit overruled.** Blijft een vlag hangen, bijvoorbeeld een RF-transactie die nooit afrondt of een onleesbare OTA-status, dan herstart het board niet. Het blijft wel opnieuw verbinden. `/api/diag` toont de reden in `restart_block` (WIFI-10).
- **Diagnose:** `/api/diag` → `wifi` heeft nu `last_outage_s`, `ip_changes` en `restart_block`. De laatste reden van verbreken gaat mee over de herstart van de watch. De grenzen staan bij de diagnose bovenaan.
- Beoordeeld in de Siri-review, ronde 3 en 4 ([`CODEX_SIRI_REVIEW.md`](CODEX_SIRI_REVIEW.md)).
