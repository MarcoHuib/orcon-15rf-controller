# Onafhankelijke beveiligingsreview van Siri (Codex) — 2026-11-17

**Wat is beoordeeld:** `feature/siri-shortcuts` ten opzichte van 3887a48 (de WiFi-reconnect, zie [`CODEX_WIFI_REVIEW.md`](CODEX_WIFI_REVIEW.md)).
- Ronde 1: e9aff80. Tegelijk zijn de WiFi-herstelpunten WIFI-05 en WIFI-06 gecontroleerd.

**Uitvoering:**
- Codex CLI 0.169 (VS Code-extensie), in een losse kloon van alleen deze branch, zonder remote.
- Niet in de kloon: `.flashtools`, captures en build-uitvoer. Alleen ArduinoJson is meegekopieerd, voor de hosttests.
- Niet gebruikt: hardware, netwerk, GitHub en GitHub Actions.

## Ronde 1 (e9aff80): GO

| ID | Ernst | Bevinding |
|----|-------|-----------|
| SIRI-01 | MINOR | `Preferences::isKey()` geeft ook bij een leesfout `false`. Bij een storing kon "Siri uitzetten" daardoor succes melden zonder dat de sleutel aantoonbaar gewist was. Was de opslag later weer leesbaar, dan werkte de oude sleutel opnieuw. Ook toonde de pagina een onleesbare sleutel als "uit". |
| SIRI-02 | MINOR | De route weigerde alleen New en Setup. Bij gedeeltelijk verlies van de opslag (Locked: geen wachtwoord meer, eigenaarsmarkering nog wel) kon een overgebleven Siri-sleutel nog bedienen, terwijl de eigenaar niet kon aanmelden om hem in te trekken. |
| SIRI-03 | INFO | Het maken en wissen van de sleutel kwam in de console, en die is zonder aanmelden leesbaar (alleen de Host-controle). De sleutel zelf stond er nooit in. |

**WIFI-05: FIXED. WIFI-06: FIXED.**

**Wel gecontroleerd en in orde:**
- **RF-bereik.** Siri kiest alleen de vijf vaste standen. Geen unit- of profielkeuze, geen identiteit, koppelen, timer of instellingen. Beide routes houden alle bestaande controles. Na het verplaatsen naar `ctlAdmit` is het gedrag van `handleRemoteSend` gelijk: volgorde, statuscodes, foutteksten en retry-antwoorden.
- **Sleutel.**
  - 128 willekeurige bits via `esp_fill_random`, met WiFi aan.
  - Vergelijking in constante tijd bij gelijke lengte.
  - Na vervangen of wissen telt direct wat in NVS staat; er is geen kopie in RAM.
  - De fabrieksreset wist `orconauth`.
  - Een leesbaar bewaarde sleutel is voor deze beperkte bevoegdheid aanvaardbaar.
- **Netwerk.**
  - Alleen IPv4.
  - AP-modus en vreemde subnetten worden geweigerd.
  - Geen DNS-rebinding-route; een GET (linkvoorbeeld) bereikt de POST-handler nooit.
  - SNAT of een proxy in het thuisnetwerk is niet te onderscheiden; dat staat al in `SIRI.md`.
- **Parsing.** Dubbele argumenten en form-bodies leveren geen route zonder sleutel en geen extra stand op.
- **Pagina.** Geen nieuw XSS-pad. De sleutel staat niet in de markup. Vervangen en uitzetten vragen eerst om bevestiging.
- **Bekend en aanvaard:** een Siri-commando verhoogt het gedeelde volgnummer. Een webverzoek dat op hetzelfde moment openstaat, krijgt dan `stale-request`, net als bij twee open browsertabbladen.

**Tests van de reviewer:**
- Alle bereikte C++-suites slagen, waaronder alle 11 HTTP-configuraties.
- De paginatests slagen: 144 checks.
- De gegenereerde header klopt met de HTML.
- `test_sx1276_tx.py` kon niet draaien, omdat RadioLib in de kloon ontbrak.

## Herstel na ronde 1
- **SIRI-01:**
  - `authstore::siriKey()` kent nu drie toestanden, net als het wachtwoordrecord: `Absent`, `Ok`, `Unreadable`. Een onjuist formaat telt als onleesbaar.
  - `setSiriKey()` schrijft en wist via de NVS-functies zelf en controleert `nvs_commit`.
  - Wissen telt alleen als het teruglezen precies `ESP_ERR_NVS_NOT_FOUND` geeft.
  - `GET /api/siri/key` geeft bij onleesbare opslag 503 `storage-unavailable`, nooit "uit". Een "uit" dat de opslag niet kan bevestigen, geeft ook 503.
  - Op `/api/siri` weigeren afwezig en onleesbaar allebei met hetzelfde 401-antwoord. Zo hoort een client zonder sleutel het verschil niet.
- **SIRI-02:** Siri werkt alleen in Done en Legacy. New, Setup en Locked krijgen 409.
- **SIRI-03:** geen consolemelding meer bij het maken of wissen van de sleutel.
- **Tests:** vier nieuwe checks in `setup_web_control`, samen 34 Siri-checks.

**Niet getest:** HTTP en WiFi in het echt, de hardware-RNG, flashfouten en stroomuitval, RF, en iOS (Opdrachten en Siri).

## Ronde 2 (9516a01): GO
**SIRI-01: FIXED. SIRI-02: FIXED. SIRI-03: FIXED.** Geen regressie gevonden in de RF-controles, de netwerk- en Host-controles, aanmelden en CSRF, het vervangen en intrekken van de sleutel, of de pagina.

Voor SIRI-01 heeft Codex de NVS-code getoetst aan het `nvs.h`-contract van ESP-IDF 4.4.7:
- **Lezen.** Een ontbrekende namespace of sleutel geeft `NOT_FOUND`. Een te lange waarde geeft `INVALID_LENGTH`, en een onjuist formaat faalt de validatie.
- **Schrijven en wissen.** Set en erase worden gecontroleerd, en `nvs_commit` moet slagen. Daarna leest de code opnieuw. Alleen een `NOT_FOUND` bij het wissen telt als succes, en ook dan moet het teruglezen bevestigen dat de sleutel weg is.

| ID | Ernst | Bevinding |
|----|-------|-----------|
| SIRI-04 | INFO | De nepversie van de opslag in de hosttest was soepeler dan de firmware. Een ongeldige opgeslagen waarde gold als geldig, en een wijziging gold als gelukt zonder teruglezen. De hosttests vervangen `auth_store.cpp` en kunnen fouten in de NVS-aanroepen dus niet vinden. |

**Herstel:**
- De nepversie volgt nu hetzelfde contract: een ongeldige opgeslagen waarde is onleesbaar, en een wijziging telt alleen na teruglezen.
- Twee nieuwe checks, samen 36 Siri-checks:
  - "uit" wordt niet geclaimd zolang het resultaat niet terug te lezen is;
  - een beschadigde opgeslagen sleutel werkt nooit als sleutel.
- De strengere nepversie ving meteen een te strenge bewering in de eigen test: na een onbevestigd wissen is de toestand onbekend. Dat is verbeterd.

**Bewust niet gedaan:** de echte NVS-code op de host draaien via een NVS-shim. Die code is door review gecontroleerd en wordt op de Heltec getest.
