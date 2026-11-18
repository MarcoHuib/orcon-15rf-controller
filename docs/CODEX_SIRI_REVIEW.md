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

## Ronde 3 (4eeb774): GO, drie eigenaarsgaranties PARTIAL
Deze ronde ging over de drie garanties die de eigenaar op 18-11 vroeg:
- de Siri-sleutel nooit in een URL of onnodig in logs, met het HTTP-risico duidelijk beschreven;
- de WiFi-herstart onderbreekt nooit een OTA, het koppelen of een ander kritiek proces;
- de diagnose scheidt een 403 van de ARP-fout.

| ID | Ernst | Bevinding |
|----|-------|-----------|
| SIRI-05 | MINOR | Na een nieuwe sleutel zeiden de pagina en de bevestigingsvraag "kopieer de adressen opnieuw". De adressen bevatten de sleutel niet meer, dus de opdrachten bleven de oude sleutel gebruiken. |
| SIRI-06 | MINOR | "Niet in logs" was te stellig. Bij een onleesbare verzoekregel of een kapotte multipart-body logt de WebServer (`log_e`, ook actief bij `CORE_DEBUG_LEVEL=2`) die regel of dat stuk naar de seriële poort. Een gewoon Siri-verzoek raakt die paden niet. |
| SIRI-07 | MINOR | De tekst moest preciezer: een gedeelde opdracht bevat ook de header; passief meelezen bij WPA2 vraagt ook de aanmelding (handshake); "niets vanaf internet" geldt niet bij een proxy of router die het bronadres vervangt; en iemand met alleen de Siri-sleutel is iets anders dan een actieve aanvaller op HTTP. |
| WIFI-07 | MINOR | Het koppelen via `/api/cmd` en MQTT (`orcon::PairState::Searching`) viel buiten de herstartblokkade. Ook kon een voltooide koppeling `busy()` al vrijgeven voordat `net::loop()` de geleerde identiteit had opgeslagen, terwijl de watch eerder in de lus draaide. |
| WIFI-08 | MINOR | "Dus offline of op een ander adres" was niet bewezen. Een stil gebroken verbinding op hetzelfde adres (het access point stuurt niet door, isolatie, verlies) verklaart ARP en mDNS ook. Dat de DHCP-lease was vrijgegeven, was een aanname. |
| WIFI-09 | MINOR | De tellers geven aanwijzingen, geen sluitend bewijs: `ip_changes` telt ook wissels na een onderbreking en mist wissels bij het opstarten en bij de overgang van het setup-AP naar het thuisnetwerk; er is geen tijdstip of oud adres; alles begint opnieuw bij een herstart, ook na de herstart door de watch. |
| WIFI-10 | INFO | Een vlag die nooit vrijkomt (een RF-transactie die nooit afrondt, een onleesbare OTA-status) blokkeert de herstart voorgoed. Advies: de reden tonen, geen onvoorwaardelijke timeout. |

**Wel in orde:**
- De header wordt correct verwerkt: leeg, een ander schema, extra spaties en te lange waarden falen dicht. Het schema is hoofdletterongevoelig, de sleutel exact. Bij een dubbele header gebruikt de Arduino-WebServer de laatste.
- Geen enkele route accepteert `k` als sleutel, maakt een URL met de sleutel, logt de sleutel of bewaart hem buiten `orconauth/siri`.
- De uitspraak over OTA klopt: schrijven gebeurt synchroon in `handleClient()`.
- Fabrieksreset, sleutelgeneratie en schrijfacties naar NVS en de sentinel kunnen niet samenvallen met de watch.

## Herstel na ronde 3
- **SIRI-05:** de bevestigingsvraag en de melding zeggen nu: zet de nieuwe sleutel in de header `Authorization` van elke opdracht; de adressen blijven gelijk. Een paginatest controleert dat.
- **SIRI-06 en SIRI-07:** `SIRI.md` is aangepast:
  - de uitzondering bij kapotte verzoeken (seriële poort, zonder sleutel bij een gewoon Siri-verzoek);
  - een gedeelde opdracht bevat de header;
  - WPA2 vraagt ook de handshake;
  - de proxy/SNAT-voorwaarde bij "niets vanaf internet";
  - een aparte alinea over een actieve aanvaller (zoals SEC-04, pas HTTPS sluit dit af).
- **WIFI-07:**
  - `restartBlock()` blokkeert ook bij `PairState::Searching`.
  - `staWatch()` draait in `loop()` pas ná het opslaan van een geleerde identiteit (`idsDirty`, `takeLearnedFan`).
  - Vlak vóór een nieuwe poging wordt de status nog eens gelezen, zodat een net herstelde verbinding niet wordt verbroken.
- **WIFI-08:** de diagnose in `CODEX_WIFI_REVIEW.md` geeft nu drie hypotheses: offline, ander adres, of een stil gebroken verbinding. Alleen "403 verklaart de ARP-fout niet" is als bewezen gemarkeerd.
- **WIFI-09:** de grenzen van de tellers staan bij de diagnose. De laatste reden van verbreken gaat nu via RTC-geheugen mee over de herstart van de watch.
- **WIFI-10:** `restartBlock()` geeft een reden (`update`, `menu`, `pairing`, `rf-or-session`), en `/api/diag` toont die als `restart_block`. Er is bewust geen timeout die de blokkade overruled.

## Ronde 4 (e4f4dcf): GO, alle drie de eigenaarsgaranties MET
Geen nieuwe bevindingen en geen regressie.

**SIRI-05, SIRI-06, SIRI-07, WIFI-07, WIFI-08, WIFI-09 en WIFI-10: alle FIXED.**

Eigenaarsgaranties:
1. **Siri-sleutel nooit in een URL of onnodig in logs, HTTP-risico duidelijk beschreven: MET.** De gegenereerde adressen bevatten alleen de stand. De sleutel gaat alleen in `Authorization`. Logging bij normaal gebruik en het resterende HTTP-risico staan duidelijk beschreven.
2. **De WiFi-herstart onderbreekt geen OTA, koppelen of ander kritiek proces: MET.** De watch beschermt een lopende OTA, koppelen, RF-transacties en leessessies, en slaat een voltooide koppeling eerst op. Een klaargezette OTA-sessie (`Ready`) kan bij een herstart verloren gaan; dat staat beschreven en onderbreekt geen lopende upload.
3. **De diagnose scheidt een 403 van de ARP-fout, zonder overdrijving: MET.** De telemetrie geldt als aanwijzing, met de grenzen erbij.

**Tests van de reviewer:**
- Alle C++-hostsuites slagen, waaronder 28 WiFi-watch-checks.
- De paginatests slagen: 145 checks.
- De gegenereerde gzip klopt met de HTML.
- `git diff --check` is in orde.

Niet getest: de firmwarebuild (wel lokaal: 11/11) en de hardware.

## Na ronde 4: twee vondsten van de lokale bewijsrun
De bewijsrun voor de release (`tools/test_local.ps1 -Firmware`) draait ook `tools/test_tx_poc_safety.py`. De hosttests doen dat niet. Twee broncontroles faalden:
- **OLED-regel.** De OLED-code mag geen `WiFi.` aanroepen; zo staat het in de regel uit `heltec-v2-rx-only.yml`, die lokaal wordt uitgevoerd. De regel "WiFi: verbinden..." vroeg `WiFi.status()` op. Hij leest nu de toestand van de watch (`g_watch.down()`). De regel zelf is niet aangepast.
- **`tx_paths_safe`.** Deze controle zocht de zendpoorten lexicaal in `handleRemoteSend`. Sinds ronde 1 staan ze in `ctlAdmit()`. De controle is mee verhuisd en minstens zo streng gebleven:
  - de vaste volgorde van de poorten en `submitCtl` als laatste, in `ctlAdmit()`;
  - precies twee aanroepers;
  - login en CSRF vóór `ctlAdmit()` in `handleRemoteSend`;
  - in `handleSiri`, in deze volgorde: netwerk, Done/Legacy, de header, de sleutelcontrole, en dan `ctlAdmit()`;
  - nooit `arg("k")`.

  Er zijn zes nieuwe mutatietests bij gekomen, en alle 13 mutaties worden afgewezen. Beoordeeld in ronde 5.

## Ronde 5 (63cfc92): GO
| ID | Ernst | Bevinding |
|----|-------|-----------|
| WIFI-11 | MINOR | `g_watch.down()` wordt pas in `loop()` bijgewerkt. Tijdens het verbinden bij het opstarten (tot 15 s) toonde het OLED daardoor weer `0.0.0.0` in plaats van "WiFi: verbinden...". |
| SIRI-08 | MINOR | De controle keek naar losse woorden, niet naar handhaving. Acht gevaarlijke mutaties kwamen erdoor, zoals `\|\|` naar `&&` in de sleutelcontrole, een genegeerd `guard()`-resultaat, een verzwakte toestandscontrole, `arg( "k" )` met spaties en een derde aanroeper `ctlAdmit (` met spatie. De bestaande handlers zelf waren wel correct afgeschermd. |

**Bewijs bij deze commit:** de bewijsrun op 63cfc92 gaf 90 van 90 PASS.

## Herstel na ronde 5
- **WIFI-11:** een vlag `g_linkUp` (het station heeft een adres) begint op onwaar. `wifiConnect()` en `apRecovery()` zetten hem bij verbinden, en de watch werkt hem elke seconde bij. Het OLED leest alleen die vlag. De OLED-regel blijft zo ook zonder WiFi-aanroep.
- **SIRI-08:** `tx_paths_safe` controleert nu hele statements, met normalisatie van witruimte en in vaste volgorde, telkens met hun vroege `return`:
  - in `handleRemoteSend`: `if (!guard(d, K, 3)) return;` vóór `ctlAdmit`;
  - in `handleSiri`: de netwerkpoort, `st`, de Done/Legacy-poort, de header, `given` en de sleutelpoort, en dan pas `ctlAdmit`.

  Aanroepers en `arg("k")` worden met een witruimte-ongevoelige regex over heel `setup_web.cpp` gezocht. Er zijn zes mutatietests bij gekomen (onder meer Codex' `||`→`&&`, het genegeerde guard-resultaat, de verzwakte toestandscontrole, `arg( "k" )`, een ontbrekende `return` en `ctlAdmit (`). Alle 19 mutaties van de zendroute worden afgewezen. De gedragstests in `test_setup_web.cpp` blijven de inhoudelijke controle.
