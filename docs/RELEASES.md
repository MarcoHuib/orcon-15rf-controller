# Officiële releases (GitHub Actions) en updates

> Stand 2026-11-22. De workflow staat op `main` en v1.0.0 is de eerste gepubliceerde release. Branch `feature/release-auto-version`: bij **Run workflow** kies je alleen nog het soort release; het versienummer volgt uit de tags en releases.

Alleen de **control-firmware** (`heltec_v2_sx1276_control`) wordt gepubliceerd. De RX-only-, RF-test- en dry-run-images blijven lokale diagnose- of herstelbuilds en worden door de workflow geweigerd.

## Zo maak je een release

1. Zorg dat `main` de code bevat die je wilt uitbrengen.
2. GitHub → **Actions** → *Release control firmware (Heltec V2)* → **Run workflow**. Kies branch `main` en:
   - het **soort release**: `patch` (standaard), `minor` of `major`. Het versienummer zelf bepaalt de workflow (zie [Automatische versie](#automatische-versie));
   - het **kanaal**: `stable` (standaard) of `beta` (zie [Bètakanaal en Matter](#bètakanaal-en-matter));
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

### Automatische versie

De workflow kiest het nummer zelf (`tools/release_ci.py next`), één stap boven de **hoogste versie onder alle tags en releases** van de repository. Niet de laatste workflow-run telt, maar de echte tag- en releasegeschiedenis:

| Soort | Regel | Voorbeeld |
|---|---|---|
| `patch` (standaard) | patch + 1 | v1.0.0 → v1.0.1 → v1.0.2 → … |
| `minor` | minor + 1, patch begint weer op 0 (SemVer) | v1.0.5 → v1.1.0, daarna v1.1.1 |
| `major` | major + 1, minor 0, patch 0 | v1.4.8 → v2.0.0, daarna v2.0.1 |

- Eén berekening in `prepare`; firmware (`ORCON_RELEASE_VERSION` en de releasestempel), manifest, bundel- en factorynaam, update-index en GitHub-tag gebruiken alle dat ene getal. `verify` controleert vóór het publiceren dat het pakket precies die versie draagt (`verify-package --release`).
- Een nummer wordt nooit hergebruikt. Een draft of een tag die een mislukte `publish` achterliet, telt mee: de volgende run neemt het nummer erna.
- Een mislukte run vóór `publish` maakt geen tag of release aan. De volgende run kiest dus hetzelfde nummer opnieuw.
- Een lege geschiedenis (geen enkele versietag of release, bijvoorbeeld omdat het opvragen mislukte) wordt geweigerd: de teller begint nooit opnieuw. Ook een stap buiten 0..999 per deel wordt geweigerd; kies dan een grotere stap.
- **Drafts:** de releases-API toont drafts alleen aan een token met schrijfrechten. Daarom haalt de aparte job `history` (contents: write, geen repositorycode, alleen `git ls-remote` en `gh api`) alle tags en releases op, drafts inbegrepen, en geeft alleen het **hoogste versienummer** door aan `prepare`, dat zelf alleen leesrechten en geen token heeft. Eén getal, dus geen lijst die een omgevingslimiet kan raken. Een draft telt dus mee, ook een draft zonder tag. `publish` haalt alles vlak vóór de tag nog eens op en stopt als het nummer intussen bezet of niet meer het hoogste is. De tag zelf wordt aangemaakt met een aanroep die faalt als hij al bestaat.
- **Namen:** alleen een naam die precies `MAJOR.MINOR.PATCH` is (met of zonder `v`) telt. `history` en de vloer in `publish` gebruiken hetzelfde awk-programma, regel voor regel, zonder iets weg te knippen.
- **Gelijktijdige runs:** er draait er één tegelijk (`concurrency: orcon-release-control`, nooit halverwege afgebroken). Een tweede run wacht en berekent zijn nummer pas als de eerste klaar is. GitHub houdt maximaal één wachtende run aan; een derde annuleert de wachtende, en die heeft dan nog niets berekend.

## Wat de workflow doet (`.github/workflows/release-control.yml`)

| Job | Rechten | Wat |
|---|---|---|
| `history` | contents: write | Alleen het ophalen van alle tags en releases, drafts inbegrepen (de API toont drafts alleen met schrijfrechten), en daaruit het hoogste versienummer met de awk-regel van de vloer in `publish`. Geen checkout, geen repositorycode, geen andere stap; de guard legt de hele stap vast. |
| `prepare` | contents: read | Alleen `workflow_dispatch`, alleen `refs/heads/main` van deze repository, en de commit moet nog de kop van `main` zijn. De versie: de volgende na de hoogste tag of release, volgens het gekozen soort (patch, minor of major). |
| `build` | contents: read | Toolchain:<br>- PlatformIO 6.2.0 met hash-gepinde wheels (`tools/release/requirements-platformio.txt`) in een eigen venv (`--force-reinstall`), daarna `pip check`;<br>- na de build ook SCons van PlatformIO Core exact gelijk aan de lock;<br>- platform en bibliotheken gelijk aan `tools/release/platformio-packages.lock`.<br><br>Tests:<br>- alle host-suites, ook met AddressSanitizer + UndefinedBehaviorSanitizer;<br>- RF-veiligheidsguards en workflowpins;<br>- privacycontrole en CLI-tests.<br><br>Daarna de releasebuild met `FW_VERSION=<commit>` en `ORCON_RELEASE_VERSION=<nummer>`, en de controles daarop:<br>- het volledige ESP-image (segmenten, checksum, appended SHA-256, niets erachter), en het image byte voor byte gelijk aan wat de vastgepinde esptool van de gecontroleerde ELF maakt;
- flashlayout;<br>- slotgrootte;<br>- precies één releasestempel;<br>- commitstempel;<br>- control-markers via de **eigen beleidscode van het apparaat** (`test/release_accept.cpp`);<br>- bootloader, partitietabel en `boot_app0` byte-gelijk aan de gepinde bestanden;<br>- rollback-hook en TX-bereikbaarheid in de ELF;<br>- de volledige pipeline-test op de echte build;<br>- factory-image. |
| `sign` | contents: read, **environment `orcon-production-release`** | De enige job die de sleutel kan lezen, en alleen in één stap:<br>- controleert dat het artifact het geteste image is;<br>- maakt het v2-manifest opnieuw uit dat image;<br>- tekent.<br><br>De sleutel bestaat alleen tijdens die stap, als 0600-bestand in een 0700-map, en wordt altijd verwijderd. Een sleutel die niet bij `src/ota_pubkey.h` hoort, wordt geweigerd. |
| `verify` | contents: read | Controleert:<br>- de handtekening (openssl, `src/ota_pubkey.h`);<br>- het apparaatbeleid op de bundel;<br>- dat een nieuwere controller de release als *ouder* weigert;<br>- de update-index zoals de controller die leest (*nieuwer* voor een oudere controller, *up-to-date* voor deze versie);<br>- `SHA256SUMS`. |
| `publish` | **contents: write** | Draait geen repositorycode. Eerst:<br>- `SHA256SUMS` en de releasenotes tegen de digests die `verify` als job-output doorgaf;<br>- daarna `sha256sum -c`;<br>- **opnieuw de versievloer** over alle tags en releases (ook drafts): een herhaalde oude run publiceert nooit een oudere versie als *latest*.<br><br>Dan:<br>1. tag aanmaken via de API (mislukt als hij al bestaat);<br>2. **draft**-release;<br>3. de bestanden terug downloaden en **byte voor byte** vergelijken met de geverifieerde bestanden;<br>4. pas dan publiceren: het **release-ID** krijgt de geverifieerde releasetekst mee en wordt *latest*. |

Het hele pakket is opnieuw gehasht tussen elke twee jobs (job-outputs). Er draait altijd hooguit één release tegelijk (`concurrency`, nooit halverwege afgebroken).

**Wat de workflow nooit doet:**
- draaien bij push, pull request, tag, release of schema;
- secrets aan andere jobs geven;
- caches gebruiken;
- actions van derden gebruiken (alleen `actions/checkout`, `setup-python`, `upload-artifact`, `download-artifact`, vastgepind op commit-SHA);
- `${{ }}` in scripts zetten (invoer gaat via `env`);
- hardware aanraken.

`tools/test_tx_poc_safety.py` pint het bestand (SHA-256) en controleert al deze eigenschappen op de geparste YAML:
- exact gepinde action-commits;
- een allowlist van alle `${{ }}`-expressies;
- het woord *secrets* precies één keer;
- geen `||`, `&&`, `!`, `set +e`, commentaarregels of `exit 0` in scripts;
- elke verplichte controle als eigen scriptregel;
- geen escapes in quoted YAML.

Daarnaast staat elke verplichte controle als exacte eigen regel. Alle besturingsregels (`if`/`for`/`{`) en alle `gh`-aanroepen moeten exact gereviewd zijn. `--help`, functiedefinities, `alias`/`set`/`eval`/`source` en een kale CR zijn verboden. Een onleesbare HEAD-boom laat de controle falen. Daarnaast gelden:
- alleen ASCII in het bestand;
- geen secrets of token in `with:`;
- git-replacement-objecten uitgeschakeld.

Elke eigenschap heeft een mutatie die geweigerd moet worden (113 stuks), waaronder:
- PR- en tagtriggers;
- secret in een andere job;
- `secrets: inherit`;
- een andere branch;
- `|| true`, een uitgecommentarieerde of omgekeerde controle;
- een andere action-commit;
- `secrets[...]`, `toJSON(secrets)` of een ge-escapete expressie;
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

**Volgorde (verplicht):** eerst de migratie in [HERSTELSLEUTEL_MIGRATIE.md](HERSTELSLEUTEL_MIGRATIE.md), stap 0 tot en met 5: herstelsleutel registreren, controleren, en het herstel met het bestand bewezen. Pas daarna het secret:
```
gh secret set ORCON_OTA_SIGNING_KEY --env orcon-production-release --repo MarcoHuib/orcon-15rf-controller < ~/.orcon/ota-signing-key.pem
```
`gh` versleutelt lokaal (sealed box). De inhoud verschijnt niet in de terminal of in logs. Het lokale bestand blijft staan.

Zonder secret faalt de job `sign` met `signing key missing`. Er wordt dan niets gepubliceerd.

**Definitief (branch `feature/remove-legacy-recovery`):** de firmware zonder Legacy-paden heeft geen herstel met deze sleutel meer. Een update op een board zonder herstelsleutel wordt niet bevestigd en valt terug. Migratievolgorde: [HERSTELSLEUTEL_MIGRATIE.md](HERSTELSLEUTEL_MIGRATIE.md).

**Later (apart akkoord):** een aparte releasesleutel K_rel volgens [RELEASESLEUTEL.md](RELEASESLEUTEL.md), zodat de ontwikkel-Mac en GitHub niet dezelfde sleutel delen.

## Bètakanaal en Matter

| Kanaal | Wat de release bevat | Hoe GitHub hem toont |
|---|---|---|
| `stable` | alleen de control-firmware (`orcon-update.json`) | gewone release, *Latest* |
| `beta` (`vX.Y.Z-beta.N`) | de control-firmware én de universele Matter-firmware (`heltec_v2_matter`, geen identiteit in het image; `orcon-update-matter.json`), elk met een eigen ondertekend manifest | *pre-release*, nooit *Latest*: de stabiele release blijft voor iedereen die geen bèta wil |

- Het bètanummer volgt uit de tags: de eerste bèta van een versie is `beta.1`, elk nummer één keer. Vanaf v1.0.1 geeft `minor` + `beta` dus v1.1.0-beta.1; de stabiele v1.1.0 staat later boven al zijn bèta's.
- De update-indexen van het bètakanaal staan in de vaste pre-release `channel-beta`. Een publicatie die boven elke eerdere tag uitkomt, vervangt ze daar; de firmware zelf blijft in de versierelease. Alleen een controller met *Bèta-updates ontvangen* aan leest ze.
- De Matter-firmware wordt alleen bij een bèta gebouwd, in de vastgepinde ESP-Matter-image (`tools/build_idf.sh --release N --pre P`). `tools/release_matter_check.sh` controleert hem zoals het control-image: het image byte voor byte uit de ELF, release- en commitstempel, de Matter-marker, géén ingebouwde identiteit, de vastgepinde bootbestanden, de rollback-hook, het apparaatbeleid en één RF-pad (`check_elf_tx.py`). De workflow bouwt nooit met een identiteit; de guard in `tools/test_tx_poc_safety.py` weigert dat.
- **Controllers met v1.0.x** kennen de Matter-variant en bèta's niet: hun manifestregels weigeren `pre` en `control-matter`. Een rechtstreekse overstap via OTA bestaat dus niet. De veilige route is eerst een stabiele control-release vanaf `main` (bijvoorbeeld v1.0.2, via *Update installeren*); daarna kan de eigenaar het Matter-bestand van de bèta bij *Handmatig installeren* kiezen en de wissel bevestigen. Zonder die tussenstap alleen via USB (het factory-image; wist de instellingen).

## Bekende restpunten van het releaseproces (Codex r4, MINOR)

- PlatformIO-pakketten (framework, esptool, SCons) worden van het PlatformIO-register gehaald en pas daarna tegen de lock gecontroleerd. Er zijn nog geen eigen gepinde archief-digests.
- Tussen de laatste controle en het publiceren kan iemand met schrijfrechten de draft nog wijzigen. Alleen de eigenaar heeft die rechten. Overweeg *immutable releases* in de repository-instellingen.
- De runner-image, de patchversie van Python 3.12 en `gh`/`g++`/OpenSSL op de runner zijn niet gepind.

## Herstel bij een mislukte release

- **Mislukt vóór `publish`.** Er is niets aangemaakt. Los de oorzaak op en start opnieuw met hetzelfde soort release: de workflow kiest dan hetzelfde nummer, want er bestaat nog geen tag.
- **Mislukt in `publish`.** Er kunnen een tag en/of een draft-release bestaan. Een volgende run slaat dat nummer over (na een mislukte v1.0.1 wordt het v1.0.2); er gaat niets mis, het nummer ontbreekt alleen in de reeks. Wil je het nummer toch gebruiken, ruim dan eerst met de hand op:
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
