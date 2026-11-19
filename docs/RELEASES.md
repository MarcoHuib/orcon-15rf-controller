# Officiële releases (GitHub Actions) en updates

> Stand 2026-11-19, branch `feature/github-releases-control-ota`. Gebouwd en lokaal getest. **Nog niet live**: de workflow kan pas draaien zodra hij op `main` staat (GitHub start `workflow_dispatch` alleen voor workflows op de standaardbranch). Daarnaast ontbreekt nog het secret met de ondertekeningssleutel; zie [Sleutel](#sleutel-wat-nog-ontbreekt). Er is nog geen enkele release gepubliceerd.

Alleen de **control-firmware** (`heltec_v2_sx1276_control`) wordt gepubliceerd. De RX-only-, RF-test- en dry-run-images blijven lokale diagnose- of herstelbuilds en worden door de workflow geweigerd.

## Zo maak je een release

1. Zorg dat `main` de code bevat die je wilt uitbrengen.
2. GitHub → **Actions** → *Release control firmware (Heltec V2)* → **Run workflow**. Kies branch `main` en vul in:
   - **versie**, bijvoorbeeld `1.0.0`;
   - eventueel **korte releasenotes**: maximaal 400 bytes, zichtbaar op de controller.
3. De workflow controleert en bouwt (zie hieronder). Bij de job *Sign (protected environment)* vraagt GitHub om **goedkeuring** (*Review deployments* → *Approve*). Dat is de enige extra klik. Zonder goedkeuring wordt er niets ondertekend of gepubliceerd.
4. Na de goedkeuring tekent de workflow, verifieert het pakket opnieuw met de regels van het apparaat en publiceert de GitHub Release.
5. Je controller meldt de nieuwe versie bij de volgende dagelijkse controle, of meteen na **Systeem → Firmware → Controleer op updates**. Installeren doe je zelf met **Update installeren** ([UPDATECONTROLE.md](UPDATECONTROLE.md)). Nooit automatisch.

## Versienummers

SemVer `MAJOR.MINOR.PATCH`, elk deel 0..999, zonder voorloopnullen en zonder achtervoegsel (`-rc1`). Het releasenummer in het getekende manifest is `MAJOR*1000000 + MINOR*1000 + PATCH`:

| SemVer | releasenummer |
|---|---|
| `1.0.0` | 1000000 |
| `1.2.3` | 1002003 |
| `2.0.0` | 2000000 |

Daardoor is de getalvolgorde gelijk aan de SemVer-volgorde. Het hoogste nummer is 999.999.999, ruim binnen uint32. `0.0.0` (0) is gereserveerd voor ontwikkelbuilds. De apparaatcode (`fw_update.cpp`) en de pipeline (`release_ci.py`) gebruiken dezelfde regel, en de tests vergelijken ze op dezelfde invoer.

**Downgradebescherming.** Een controller installeert nooit een release met een lager nummer dan zijn eigen `ORCON_RELEASE_VERSION` (`older-release`). De updatecontrole biedt alleen een *hoger* nummer aan.

De huidige controller (b75fad3) is een ontwikkelbuild met nummer 0. Hij accepteert dus elke release, en `v1.0.0` is een geldige eerste release.

De workflow weigert een versie:
- als de tag of release al bestaat;
- als ze niet hoger is dan elke gepubliceerde versie.

## Wat de workflow doet (`.github/workflows/release-control.yml`)

| Job | Rechten | Wat |
|---|---|---|
| `prepare` | contents: read | Alleen `workflow_dispatch`, alleen `refs/heads/main` van deze repository, en de commit moet nog de kop van `main` zijn. Versie geldig, nieuw en hoger dan alle tags en releases. |
| `build` | contents: read | Toolchain:<br>- PlatformIO 6.2.0 met hash-gepinde wheels (`tools/release/requirements-platformio.txt`);<br>- platform en bibliotheken gelijk aan `tools/release/platformio-packages.lock`.<br><br>Tests:<br>- alle host-suites, ook met AddressSanitizer + UndefinedBehaviorSanitizer;<br>- RF-veiligheidsguards en workflowpins;<br>- privacycontrole en CLI-tests.<br><br>Daarna de releasebuild met `FW_VERSION=<commit>` en `ORCON_RELEASE_VERSION=<nummer>`, en de controles daarop:<br>- ESP-imagekop en flashlayout;<br>- slotgrootte;<br>- precies één releasestempel;<br>- commitstempel;<br>- control-markers via de **eigen beleidscode van het apparaat** (`test/release_accept.cpp`);<br>- bootloader, partitietabel en `boot_app0` byte-gelijk aan de gepinde bestanden;<br>- rollback-hook en TX-bereikbaarheid in de ELF;<br>- de volledige pipeline-test op de echte build;<br>- factory-image. |
| `sign` | contents: read, **environment `orcon-production-release`** | De enige job die de sleutel kan lezen, en alleen in één stap:<br>- controleert dat het artifact het geteste image is;<br>- maakt het v2-manifest opnieuw uit dat image;<br>- tekent.<br><br>De sleutel bestaat alleen tijdens die stap, als 0600-bestand in een 0700-map, en wordt altijd verwijderd. Een sleutel die niet bij `src/ota_pubkey.h` hoort, wordt geweigerd. |
| `verify` | contents: read | Controleert:<br>- de handtekening (openssl, `src/ota_pubkey.h`);<br>- het apparaatbeleid op de bundel;<br>- dat een nieuwere controller de release als *ouder* weigert;<br>- de update-index zoals de controller die leest (*nieuwer* voor een oudere controller, *up-to-date* voor deze versie);<br>- `SHA256SUMS`. |
| `publish` | **contents: write** | Draait geen repositorycode. Eerst `sha256sum -c`. Dan:<br>1. tag aanmaken via de API (mislukt als hij al bestaat);<br>2. **draft**-release met de bestanden;<br>3. de bestanden terug downloaden en tegen `SHA256SUMS` controleren;<br>4. pas dan publiceren als *latest*. |

Het hele pakket is opnieuw gehasht tussen elke twee jobs (job-outputs). Er draait altijd hooguit één release tegelijk (`concurrency`, nooit halverwege afgebroken).

**Wat de workflow nooit doet:**
- draaien bij push, pull request, tag, release of schema;
- secrets aan andere jobs geven;
- caches gebruiken;
- actions van derden gebruiken (alleen `actions/checkout`, `setup-python`, `upload-artifact`, `download-artifact`, vastgepind op commit-SHA);
- `${{ }}` in scripts zetten (invoer gaat via `env`);
- hardware aanraken.

`tools/test_tx_poc_safety.py` pint het bestand (SHA-256) en controleert al deze eigenschappen op de geparste YAML. Elke eigenschap heeft een mutatie die geweigerd moet worden (36 stuks), waaronder:
- PR- en tagtriggers;
- secret in een andere job;
- `secrets: inherit`;
- een andere branch;
- een dry-run-, RF-test- of RX-only-image;
- overgeslagen controles;
- write-rechten;
- niet-gepinde actions;
- cache;
- hardware.

## Bestanden in een release

| Bestand | Voor | Inhoud |
|---|---|---|
| `orcon-control-vX.Y.Z-<commit7>.orconfw` | updates (Systeem → Firmware) | regel 1 `{"manifest","sig"}` (v2-releasemanifest, ECDSA P-256), daarna het control-image |
| `orcon-update.json` | de updatecontrole van de controller | hetzelfde getekende manifest + handtekening + korte notes (≤ 2048 B) |
| `orcon-control-vX.Y.Z-<commit7>-factory-usb.bin` | **alleen een nieuwe controller**, eenmalig via USB vanaf 0x0 | bootloader, partitietabel, boot_app0 en het control-image. **Wist alle instellingen**: WiFi, wachtwoord, herstelsleutel, profielen. Nooit gebruiken voor een controller in gebruik, en nooit automatisch. |
| `BUILD_INFO.json` | controle | commit, versie, image-grootte, SHA-256 van alle onderdelen |
| `SHA256SUMS` | controle | hashes van alle bovenstaande bestanden |

## GitHub-instellingen

| Instelling | Stand | Door |
|---|---|---|
| Environment `orcon-production-release` | **aangemaakt** (2026-11-19) | Claude, via `gh api` |
| — deployment branches | **alleen `main`** (eigen branchbeleid) | Claude |
| — required reviewers | **MarcoHuib** (goedkeuring per release) | Claude |
| — admins mogen regels omzeilen | **nee** (`can_admins_bypass: false`) | Claude |
| — secret `ORCON_OTA_SIGNING_KEY` | **ONTBREEKT** (bewust niet gezet, zie hieronder) | eigenaar |
| Repository-secrets en -variabelen | geen (en zo laten) | — |
| Actions | aan; standaard workflowrechten *read*; PR-goedkeuring door Actions uit | ongewijzigd |
| Branchbescherming `main` | geen (alleen de eigenaar heeft schrijfrechten) | ongewijzigd, zie aanbeveling |

Wil je de extra goedkeuringsklik niet, verwijder dan de reviewer onder Settings → Environments → `orcon-production-release`. Het branchbeleid moet blijven.

## Sleutel: wat nog ontbreekt

**Feiten (gecontroleerd):**
- `~/.orcon/ota-signing-key.pem` hoort bij `src/ota_pubkey.h`: SPKI-SHA-256 `e71543f1…`.
- `ota_pubkey.h` is ongewijzigd sinds bb80bb3. De firmware op het board (b75fad3 in app1, 2edb423 in app0) vertrouwt dus precies deze sleutel.
- Een release moet met deze sleutel getekend zijn, anders weigert de huidige controller hem. Daarom wordt er geen nieuwe sleutel gebruikt en niet geroteerd.

**Waarom het secret nog niet is gezet.** Op een controller in de toestand *legacy* (gemigreerd, nog geen herstelsleutel van de eigenaar) is deze sleutel méér dan een firmwaresleutel. Wie hem heeft, kan via `orcon.py auth-code` (`/api/auth/nonce` + `/api/auth/reset`, `vendorRecoveryOk()` in `setup_web.cpp`) vanaf het thuisnetwerk een herstelcode krijgen en het beheerderswachtwoord vervangen. Jouw controller meldt `legacy`.

Staat de sleutel in GitHub, dan geeft een lek daar (plus toegang tot je netwerk) toegang tot je controller. Dat valt buiten de opdracht "alleen als bestaande apparaten zonder rotatie veilig blijven". Een lek kan op drie manieren ontstaan:
- een kwaadaardige commit op `main`;
- een gecompromitteerde action;
- GitHub zelf.

**Kies een van twee:**
1. **Aanbevolen.** Registreer eerst een herstelsleutel op de controller: RST, dan PRG 15 s → Herstelmodus, zie [INSTALLATIE.md](INSTALLATIE.md). De toestand wordt dan *done* en de releasesleutel geeft geen toegang meer (`owner-key-required`). Zet daarna het secret:
   ```
   gh secret set ORCON_OTA_SIGNING_KEY --env orcon-production-release --repo MarcoHuib/orcon-15rf-controller < ~/.orcon/ota-signing-key.pem
   ```
   `gh` versleutelt lokaal (sealed box). De inhoud verschijnt niet in de terminal of in logs. Het lokale bestand blijft staan.
2. Je accepteert dit risico bewust en zet het secret meteen met hetzelfde commando.

Zonder secret faalt de job `sign` met `signing key missing`. Er wordt dan niets gepubliceerd.

**Later (apart akkoord):** een aparte releasesleutel K_rel volgens [RELEASESLEUTEL.md](RELEASESLEUTEL.md), zodat de ontwikkel-Mac en GitHub niet dezelfde sleutel delen.

## Herstel bij een mislukte release

- **Mislukt vóór `publish`.** Er is niets aangemaakt. Los de oorzaak op en start opnieuw. Dezelfde versie mag, want er bestaat nog geen tag.
- **Mislukt in `publish`.** Er kunnen een tag en/of een draft-release bestaan, en de workflow weigert die versie dan (*already exists*). Herstel met de hand:
  1. Controleer in GitHub of de release een **draft** is. Een gepubliceerde release verwijder je niet; geef dan een nieuwe versie uit.
  2. Verwijder de draft en de tag:
     ```
     gh release delete vX.Y.Z --yes
     git push --delete origin vX.Y.Z
     ```
  3. Start de workflow opnieuw.
- **Een gepubliceerde release is fout.** Publiceer een hogere versie. Controllers installeren nooit een oudere.

## Lokaal hetzelfde controleren (zonder publiceren)

```
python3 tools/test_release_ci.py      # hele pipeline met wegwerpsleutels en gemanipuleerde invoer
python3 tools/test_tx_poc_safety.py   # workflowpin + releaseworkflow-guard + mutaties
python3 tools/test_ab_transition.py   # app0/app1 door de overgang heen (simulatie)
bash test/run_host_tests.sh           # alles, incl. updatecontrole en webinterface
```

Gebruik voor tekentests nooit de productiesleutel. De tests maken per run wegwerpsleutels.

## Eerste release: wat er op het board gebeurt

Zie [A/B-overgang](AB_OVERGANG.md). Kort samengevat:
- Het board draait b75fad3 (control) uit **app1**. De eerste release gaat dus naar **app0** en vervangt het oude RX-only-image 2edb423.
- b75fad3 blijft de terugvaloptie.
- Daarna wisselen app0 en app1 per release, steeds met de vorige control-release als terugvaloptie.

## Hardwarevalidatie (nog te doen, met toestemming per stap)

1. De eerste release installeren via **Update installeren**. Dan controleren:
   - voortgang;
   - proefstart;
   - *Update geslaagd*;
   - `/api/ota`: app0 valid en running, app1 = b75fad3 valid.
2. `/api/update` na de controle: `mem.heap_min_job` en `mem.stack_free_job` noteren (TLS-geheugen).
3. RF-bediening en Siri na de update. Profielen, WiFi en eigenaarschap moeten ongewijzigd zijn (`orcon.py backup-check`).
4. Optioneel een rollbacktest met de volgende release (`orcon.py ota-install --test-rollback`, of de hold-knop): het board moet terug naar de vorige control-release.

## Eerder genoemde releaseblokkades (besluit eigenaar)

De workflow controleert deze niet. Ze blijven gelden voor een release die ook voor anderen bedoeld is:
- HTTPS op het LAN ([HTTPS_ONTWERP.md](HTTPS_ONTWERP.md));
- hardwarevalidatie van de installatiewizard ([HW_VALIDATIE_PROVISIONING.md](HW_VALIDATIE_PROVISIONING.md));
- een aparte releasesleutel ([RELEASESLEUTEL.md](RELEASESLEUTEL.md)).

Voor een release voor je eigen controller zijn ze geen technische voorwaarde.
