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
| 5 | volgt | — | moet OWNER-10/11/12 expliciet beoordelen |

## Bevindingen en herstel

| ID | Ernst | Probleem | Herstel | Status |
|---|---|---|---|---|
| OWNER-01 | MAJOR | Een onleesbare resetmarker gold als "geen reset"; daardoor kon Legacy-herstel terugkomen | De marker wordt in drie toestanden gelezen; onleesbaar stopt het normale opstarten. Wisvolgorde: eerst `orconauth`, dan `orconown` | FIXED (r2, r3) |
| OWNER-02 | MAJOR | Een onleesbaar wachtwoord heropende stap 1 | `authstore::status()` geeft drie toestanden; onleesbaar telt als "gezet" | FIXED (r3) |
| OWNER-03 | MAJOR | Herstelcodes van de releasesleutel overleefden de registratie van een eigenaarssleutel | `/api/auth/setup` werkt alleen nog in Legacy; codes vervallen bij registratie | FIXED (r3) |
| OWNER-04 | MINOR | De herstelmodus kwam terug na de wrap van `millis()` | `provision::PhysicalTimer` blijft na afloop uit | FIXED (r2) |
| OWNER-05 | MINOR | De Arduino-WebServer buffert hele POST-bodies vóór de handlers (geërfd) | Niet opgelost; gedocumenteerd (alleen DoS) | ACCEPTED |
| OWNER-06 | MAJOR | Eerste wachtwoord opgeslagen, teruglezen mislukt: stap 1 bleef open | RAM neemt over wat in de opslag staat; stap 1 opent alleen als de opslag op dat moment bevestigt dat er geen wachtwoord is | FIXED (r3) |
| OWNER-07 | MAJOR | Registratie dubbelzinnig mislukt: Legacy-herstel bleef open | Codes vervallen vóór het schrijven; Legacy sluit in RAM; een opgeslagen sleutel betekent altijd Done | FIXED (r3) |
| OWNER-08 | MAJOR | Wachtwoordvervanging dubbelzinnig mislukt: het oude wachtwoord en oude sessies bleven geldig | Alle sessies afgemeld; niets meer gecachet | FIXED (r4) |
| OWNER-09 | MAJOR | Een leesfout op het rollbackimage gold als "geen image" | Alleen `ESP_ERR_IMAGE_INVALID` telt als niet opstartbaar; bij andere fouten blijft de marker staan | FIXED (r4) |
| OWNER-10 | MAJOR | Een onleesbare OTA-proefstaat stond een herstart vóór het wissen toe; daarna (r4) ook een leesbare INVALID/ABORTED- of onbekende staat | `unsettledBoot()` met een allowlist: alleen VALID en UNDEFINED zijn stabiel | r4 PARTIAL; allowlist toegevoegd; **ronde 5 nodig** |
| OWNER-11 | MAJOR | De Arduino-NVS-autoformat wiste eigendom en de resetmarker, waardoor de wizard weer openging | Eigendomsmarkering buiten NVS (twee sectoren in de ongebruikte `spiffs`-partitie, A/B, CRC). De resetmarker staat daar ook. NVS leeg terwijl de markering een eigenaar toont: staat **Locked**, en alleen de PRG-fabrieksreset helpt | gerepareerd; host: 34 extra checks (66 onderbrekingspunten); **ronde 5 nodig** |
| OWNER-12 | MINOR | De controle op privésleutels miste compacte PEM en een PEM in één escaped string | Controle op hele PEM-blokken, ook over escaped regeleinden en string-lijm heen; `tools/test_check_privacy.py` met echte wegwerpsleutels in 7 vormen | gerepareerd; **ronde 5 nodig** |

## Open

**Ronde 5**
- Verplicht vóór een merge.
- Te beoordelen:
  - OWNER-10 (allowlist), OWNER-11 (eigendomsmarkering buiten NVS, staat Locked, resetmarker daar) en OWNER-12 (controle op privésleutels) expliciet;
  - geen terugkeer van OWNER-01–09.

**Geaccepteerd en gedocumenteerd**
- OWNER-05: de WebServer buffert POST-bodies; alleen een DoS.
- Na een NVS-autoformat is de configuratie van de eigenaar weg. Dat is gegevensverlies, geen toegangsverlies: het apparaat wordt Locked.

**Hardware.** Alles hierboven is host- en probe-bewijs. Zie [`HW_VALIDATIE_PROVISIONING.md`](HW_VALIDATIE_PROVISIONING.md).
