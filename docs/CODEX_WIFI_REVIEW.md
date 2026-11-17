# Onafhankelijke review van de WiFi-reconnect (Codex) — 2026-11-17

**Wat is beoordeeld:** `feature/wifi-reconnect` ten opzichte van 1a92fb0 (de `src/` van 2edb423, Codex ronde 7 GO).
- Ronde 1: dbeed9e.

**Aanleiding:** de Heltec was onbereikbaar. Het ARP-record op de Mac bleef `incomplete`, `orcon15rf.local` werd niet gevonden en het OLED toonde nog het oude IP-adres. Na een RST werkte alles weer, met een ander DHCP-adres. Er was geen crashdump. De precieze oorzaak is achteraf niet vast te stellen. Er zijn twee mogelijkheden, en beide zijn in de code aan te wijzen:
1. De Arduino-kern (2.0.16) probeert alleen opnieuw te verbinden bij redenen die op zijn eigen lijst staan. Na de eerste mislukking stopt hij voorgoed bij elke andere reden, bijvoorbeeld 202 AUTH_FAIL terwijl een access point opstart.
2. `g_ip` was een momentopname bij het opstarten. Na een reconnect met een nieuw DHCP-adres toonde het OLED dus nog het oude adres, en weigerde de Host-controle elk verzoek naar het nieuwe adres met 403.

Een ESP32 met verbinding stuurt elke 60 s een gratuitous ARP (lwIP-instelling). Was het board op het oude adres verbonden geweest, dan was het `incomplete`-record vanzelf opgelost.

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
