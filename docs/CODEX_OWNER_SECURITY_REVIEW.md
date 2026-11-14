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
| 4 | d5d2d9c | **niet afgerond**: gebruikslimiet van het Codex-account bereikt | geen uitslag; zie "Open" |

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
| OWNER-08 | MAJOR | Wachtwoordvervanging dubbelzinnig mislukt: het oude wachtwoord en oude sessies bleven geldig | Alle sessies afgemeld; niets meer gecachet | gerepareerd in d5d2d9c; hosttest met mutatie; **door Codex niet meer geverifieerd** |
| OWNER-09 | MAJOR | Een leesfout op het rollbackimage gold als "geen image" | Alleen `ESP_ERR_IMAGE_INVALID` telt als niet opstartbaar; bij andere fouten blijft de marker staan | gerepareerd in d5d2d9c; alleen apparaatcode; **niet geverifieerd** |
| OWNER-10 | MAJOR | Een onleesbare OTA-proefstaat stond een herstart vóór het wissen toe | `unsettledBoot()` faalt dicht; menu-acties wachten | gerepareerd in d5d2d9c; alleen apparaatcode; **niet geverifieerd** |

## Open

**Ronde 4**
- Opnieuw draaien zodra het Codex-account weer beschikbaar is. Doel: OWNER-08–10 laten bevestigen en het platformgedrag hieronder laten beoordelen.

**Platform: NVS-autoformat door Arduino-ESP32** (`esp32-hal-misc.c`, `initArduino`)
- Bij `ESP_ERR_NVS_NO_FREE_PAGES` of `ESP_ERR_NVS_NEW_VERSION_FOUND` wist de core de hele NVS vóór `setup()`. Dat neemt de resetmarker en alle eigendomsgegevens tegelijk mee.
- Het apparaat start dan als New. Er is geen eigendom meer en het setup-AP krijgt een nieuw wachtwoord.
- **Opnieuw in bezit nemen** kan alleen met fysieke toegang (het wachtwoord staat alleen op het OLED). Op afstand kan dit niet worden uitgelokt: elke schrijfroute vraagt een sessie.
- **Gat 1: de stap "pre-owner-image wissen" van een fabrieksreset draait niet** als de marker zo verdwijnt. Dat is alleen relevant zolang er nog een image van vóór dit model in de andere sleuf staat, dus alleen tijdens de migratie. De migratievolgorde in `PROVISIONING.md` §7 (beide OTA's vóór de sleutelregistratie) voorkomt die situatie.
- **Gat 2: de eigenaar verliest zijn configuratie** zonder knopbediening. Dat is gegevensverlies, geen toegangsverlies.
- Als dit een harde eis wordt: een image van vóór dit model ook bij de overgang naar Done wissen, als de draaiende firmware bevestigd is. Dat is niet gebouwd, omdat de gedocumenteerde migratievolgorde het al voorkomt.

**Hardware.** Alles hierboven is host- en probe-bewijs. Zie [`HW_VALIDATIE_PROVISIONING.md`](HW_VALIDATIE_PROVISIONING.md).
