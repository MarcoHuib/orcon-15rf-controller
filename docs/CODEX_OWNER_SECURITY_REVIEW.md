# Onafhankelijke beveiligingsreview van het eigendomsmodel (Codex) — 2026-11-14

**Wat is beoordeeld:** branch `feature/plug-and-play-setup` ten opzichte van `feature/production-control` (f2e8621).

**Uitvoering**
- Codex CLI 0.169, in een losse kloon met alleen de publieke branchgeschiedenis.
- Sandbox: schrijven alleen in die kloon.
- Niet gebruikt: hardware, seriële poort, netwerk, GitHub en GitHub Actions.
- Codex bewees bevindingen met probes. Die bouwden de echte bronbestanden met nagebootste NVS-, flash- en SDK-fouten (`.review-probes/` in de reviewkloon).

| Ronde | Commit | Uitslag | Bevindingen |
|---|---|---|---|
| 1 | 8bbc2b3 | NO-GO | OWNER-01..03 MAJOR, OWNER-04 MINOR, OWNER-05 MINOR (geërfd) |
| 2 | 1cd4581 | NO-GO | 01, 04 FIXED; 02, 03 PARTIAL; nieuw: OWNER-06/07 MAJOR |
| 3 | 014e9e0 | NO-GO | 01–04, 06, 07 FIXED; nieuw: OWNER-08/09/10 MAJOR |
| 4 (poging) | d5d2d9c | niet afgerond: gebruikslimiet van het Codex-account | geen uitslag |
| 4 | 830b603 | NO-GO | 01–09 FIXED, 05 ACCEPTED; 10 PARTIAL; nieuw: OWNER-11 MAJOR, OWNER-12 MINOR |
| 5 | 6605be6 | NO-GO | 02–09 FIXED, 05 ACCEPTED; 01, 10, 11, 12 PARTIAL; nieuw: OWNER-13 MINOR, OWNER-14 INFO |
| 6 | volgt | — | moet OWNER-01, 10–14 expliciet beoordelen |

## Bevindingen en herstel

| ID | Ernst | Probleem | Herstel | Status |
|---|---|---|---|---|
| OWNER-01 | MAJOR | Een onleesbare resetmarker gold als "geen reset"; daardoor kon Legacy-herstel terugkomen. R5: een onderbroken noodreset liet "beschadigd + leeg" achter, en dat gold als "geen marker" | De marker wordt in drie toestanden gelezen; onleesbaar stopt het normale opstarten. Wisvolgorde: eerst `orconauth`, dan `orconown`. R5: beschadigd naast leeg is onleesbaar, nooit afwezig; de noodreset schrijft eerst de onbeschadigde sector | FIXED (r2, r3); r5 PARTIAL; gerepareerd, **ronde 6 nodig** |
| OWNER-02 | MAJOR | Een onleesbaar wachtwoord heropende stap 1 | `authstore::status()` geeft drie toestanden; onleesbaar telt als "gezet" | FIXED (r3) |
| OWNER-03 | MAJOR | Herstelcodes van de releasesleutel overleefden de registratie van een eigenaarssleutel | `/api/auth/setup` werkt alleen nog in Legacy; codes vervallen bij registratie | FIXED (r3) |
| OWNER-04 | MINOR | De herstelmodus kwam terug na de wrap van `millis()` | `provision::PhysicalTimer` blijft na afloop uit | FIXED (r2) |
| OWNER-05 | MINOR | De Arduino-WebServer buffert hele POST-bodies vóór de handlers (geërfd) | Niet opgelost; gedocumenteerd (alleen DoS) | ACCEPTED |
| OWNER-06 | MAJOR | Eerste wachtwoord opgeslagen, teruglezen mislukt: stap 1 bleef open | RAM neemt over wat in de opslag staat; stap 1 opent alleen als de opslag op dat moment bevestigt dat er geen wachtwoord is | FIXED (r3) |
| OWNER-07 | MAJOR | Registratie dubbelzinnig mislukt: Legacy-herstel bleef open | Codes vervallen vóór het schrijven; Legacy sluit in RAM; een opgeslagen sleutel betekent altijd Done | FIXED (r3) |
| OWNER-08 | MAJOR | Wachtwoordvervanging dubbelzinnig mislukt: het oude wachtwoord en oude sessies bleven geldig | Alle sessies afgemeld; niets meer gecachet | FIXED (r4) |
| OWNER-09 | MAJOR | Een leesfout op het rollbackimage gold als "geen image" | Alleen `ESP_ERR_IMAGE_INVALID` telt als niet opstartbaar; bij andere fouten blijft de marker staan | FIXED (r4) |
| OWNER-10 | MAJOR | Een onleesbare OTA-proefstaat stond een herstart vóór het wissen toe; daarna (r4) ook een leesbare INVALID/ABORTED- of onbekende staat | `unsettledBoot()` met een allowlist: alleen VALID en UNDEFINED zijn stabiel. R5: ook de foutlus (onleesbare opslag) volgt die regel: tijdens een proefstart geen automatische herstart en geen noodreset | r4, r5 PARTIAL; gerepareerd, **ronde 6 nodig** |
| OWNER-11 | MAJOR | De Arduino-NVS-autoformat wiste eigendom en de resetmarker, waardoor de wizard weer openging | Eigendomsmarkering buiten NVS (twee sectoren in de ongebruikte `spiffs`-partitie, CRC). De resetmarker staat daar ook. NVS leeg terwijl de markering een eigenaar toont: staat **Locked**, en alleen de PRG-fabrieksreset helpt. R5: elke wijziging in beide sectoren (geen oude toestand die terug kan komen), een beschadigde magic telt als beschadigd, en het herstel bij het opstarten maakt beide sectoren weer gelijk | r5 PARTIAL; gerepareerd (pure logica in `provision.cpp`, host: 2.860 onderbrekingspunten en schade aan elke bit); **ronde 6 nodig** |
| OWNER-12 | MINOR | De controle op privésleutels miste compacte PEM en een PEM in één escaped string | Controle op hele PEM-blokken, ook over escaped regeleinden en string-lijm heen; `tools/test_check_privacy.py` met echte wegwerpsleutels. R5: alleen een `+` tússen string-literals is lijm, een `+` in base64 blijft staan; Ed25519/X25519 (de kortste sleutels) in drie vormen getest | r5 PARTIAL; gerepareerd, **ronde 6 nodig** |
| OWNER-13 | MINOR | Een eerste-wachtwoordverzoek in de 1,5 s tussen het bevestigen van de reset en de herstart wiste de klaarstaande reset | Stap 1 en het herstel bij het opstarten weigeren bij een klaarstaande reset; na het bevestigen herstart het apparaat direct (blokkerend) | gerepareerd; **ronde 6 nodig** |
| OWNER-14 | INFO | Bij een volgnummer op UINT32_MAX meldde wissen succes terwijl het oude record bleef gelden | Vergelijking met wrap-around; na elke schrijfactie wordt de gekozen toestand teruggelezen | gerepareerd; **ronde 6 nodig** |

## Open

**Ronde 6**
- Verplicht vóór een merge.
- Te beoordelen:
  - OWNER-01, 10, 11, 12, 13 en 14 expliciet;
  - geen terugkeer van OWNER-02–09.

**Geaccepteerd en gedocumenteerd**
- OWNER-05: de WebServer buffert POST-bodies; alleen een DoS.
- Na een NVS-autoformat is de configuratie van de eigenaar weg. Dat is gegevensverlies, geen toegangsverlies: het apparaat wordt Locked.

**Hardware.** Alles hierboven is host- en probe-bewijs. Zie [`HW_VALIDATIE_PROVISIONING.md`](HW_VALIDATIE_PROVISIONING.md).
