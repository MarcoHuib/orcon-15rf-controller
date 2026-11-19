# Releases en GitHub-integratie (voorstel)

Dit is een voorstel. Er is nog niets gepubliceerd. Publiceren gebeurt alleen na een expliciet besluit, per release.

## Randvoorwaarden

- **Nu geen GitHub Actions.** Bouwen, testen en ondertekenen gebeurt lokaal. Dat sluit ook GitHub Pages met "deploy from a branch" uit, want dat draait intern de Actions-workflow `pages-build-deployment`.
- **Besluit (2026-11-19, nog niet gebouwd).** Officiële releases komen straks uit GitHub Actions: een GitHub Release met versienummer, de firmware automatisch gebouwd en getest, ondertekend, en het ondertekende `.orconfw`-pakket gepubliceerd. De Heltec meldt dat er een update is en installeert pas na toestemming van de aangemelde eigenaar. De private OTA-signingsleutel staat nooit in de repository en is nooit beschikbaar voor gewone push-, PR- of testworkflows: alleen de gecontroleerde ondertekeningsjob van een officiële release leest hem, als secret van een beveiligde GitHub Environment. Tot dan geldt het lokale proces in dit document.
- **Workflows nu.** De enige workflow (`.github/workflows/heltec-v2-rx-only.yml`) is uitgeschakeld: hij reageert alleen op `workflow_dispatch` en stopt dan meteen met een foutmelding, zonder iets te bouwen, te testen of te uploaden. Een push, tag of release start dus niets. `tools/test_tx_poc_safety.py` pint elk workflowbestand op inhoud (`WORKFLOWS`) en faalt bij een gewijzigde of nieuwe workflow. Die controle draait lokaal, vóór elke push; GitHub raadpleegt hem niet. Een handmatige start gebruikt het workflowbestand van de gekozen branch of tag: vóór de merge van PR #15 heeft `main` nog de oude workflow, daarna geen enkele ref meer (stand 2026-11-19: alleen `main` en de PR-branch, geen tags). Een toekomstige release-workflow wordt bewust toegevoegd: eerst eigen beveiligingscontroles en een review, dan opname in die lijst; voor de sleutel geldt het besluit hierboven. Nu staat de sleutel alleen lokaal en heeft de repository geen secrets, variabelen of environments. Controleer dit opnieuw vóór elke publicatie.
- **Reproduceerbaar.** De bouw is vastgepind: platform `espressif32@6.7.0` en de lib-versies in `platformio.ini`. Het lokale bewijspakket werkt zoals in [`LOCAL_USB_RELEASE.md`](LOCAL_USB_RELEASE.md).

## Wat een release bevat (per versie N)

| Bestand | Voor wie | Inhoud |
|---|---|---|
| `orcon-controller-heltec-v2-factory.bin` | eerste installatie, eenmalig via USB | Eén bestand vanaf adres 0x0 (`esptool merge_bin`):<br>- vastgepinde bootloader (0x1000);<br>- partitietabel (0x8000);<br>- `boot_app0.bin` (0xe000);<br>- het control-image (0x10000). |
| `orcon-control-N-<commit>.orconfw` | updates via de webpagina | v2-releasemanifest met handtekening, plus het control-image |
| `orcon-rx-only-N-<commit>.orconfw` | wie liever alleen ontvangt | v2-bundel met het RX-only-image |
| `SHA256SUMS` | controle | hashes van alle bestanden |
| releasenotes | iedereen | commit, versie, testbewijs, NOT TESTED-lijst, migratie-aanwijzingen |

**Controle op het apparaat.** De bundels maak je met `tools/orcon.py release --worktree <wt> --env <env> --version N --yes [--tx-ack "control firmware"]`. Het apparaat accepteert een bundel alleen als:
- de releasesleutel klopt;
- de versie niet lager is dan de eigen versie;
- de aangemelde eigenaar hem installeert.

**Versie in het image.** Het image moet als release N gebouwd zijn: `-DORCON_RELEASE_VERSION=N`, naast `-DFW_VERSION="<commit>"` in `PLATFORMIO_BUILD_FLAGS`. `orcon.py release` controleert dat aan het stempel `ORCON-RELEASE N;` in het image.

**Nog te doen.** De bewijsbouw (`tools/test_local.ps1` en de bash-variant) geeft nu alleen `FW_VERSION` door. Een releasebouw moet daar `ORCON_RELEASE_VERSION` aan toevoegen.

## Eerste installatie zonder terminal

**Aanbevolen.** De officiële webflasher van Espressif: <https://espressif.github.io/esptool-js/>.
- Werkt in Chrome en Edge via WebSerial.
- Gebruiker: Connect → kies `…-factory.bin` op 0x0 → Program.
- Geen eigen hosting nodig.

**Later, optioneel.** Een eigen pagina met ESP Web Tools (één knop, met een manifest).
- Zolang er geen Actions draaien, moet die buiten GitHub Pages gehost worden: Pages draait Actions. Met de release-workflow uit het besluit kan dat opnieuw worden afgewogen.
- Een externe statische host is een nieuwe afhankelijkheid en vraagt een apart besluit.

## Updates

**Nu: handmatig.** De eigenaar downloadt `.orconfw` en installeert die via Systeem → Firmware.

**Mogelijk later.** Een updatecontrole op het apparaat tegen de GitHub-releases-API.
- Nadelen: HTTPS met een CA-bundel (tientallen KB flash); het apparaat neemt contact op met GitHub (privacy); en een nieuwe afhankelijkheid.
- Installeren blijft dan altijd een handeling van de aangemelde eigenaar.

## Releaseblokkades (vóór de eerste publieke release)

1. **HTTPS op het LAN.** Een wachtwoord via HTTP is niet voldoende. Ontwerp: [`HTTPS_ONTWERP.md`](HTTPS_ONTWERP.md).
2. **Hardwarevalidatie**: [`HW_VALIDATIE_PROVISIONING.md`](HW_VALIDATIE_PROVISIONING.md).
3. **Een groene laatste onafhankelijke securityreview**: [`CODEX_OWNER_SECURITY_REVIEW.md`](CODEX_OWNER_SECURITY_REVIEW.md).
4. **Een eigen releasesleutel**, gescheiden van de ontwikkelsleutel, ingevoerd met de migratie in [`RELEASESLEUTEL.md`](RELEASESLEUTEL.md); het privé-deel straks alleen als secret van een beveiligde GitHub Environment (besluit hierboven). Alleen met apart akkoord.

## Releasesleutel: aparte sleutel besloten, migratie ontworpen

Het ontwerp staat in [`RELEASESLEUTEL.md`](RELEASESLEUTEL.md). De tekst hieronder is de oorspronkelijke analyse. Sinds het besluit van 2026-11-19 tekent straks een gecontroleerde GitHub-releasejob met de sleutel uit een beveiligde Environment; de offline-ceremonie in dat ontwerp wordt in de releasefase daarop aangepast.

**Huidige situatie**
- De publieke sleutel in `src/ota_pubkey.h` hoort bij `~/.orcon/ota-signing-key.pem` op de ontwikkel-Mac.
- Die sleutel is daarmee de releasesleutel voor elk apparaat dat deze firmware draait.

**Advies vóór de eerste publieke release**
- Maak een aparte releasesleutel die niet op de dagelijkse ontwikkelmachine staat, bijvoorbeeld versleuteld en offline.
- Zet die publieke sleutel in de firmware.
- Wisselen kan nu nog eenvoudig, want er zijn nog geen publieke apparaten.
- Later wisselen vraagt een overgangsrelease, getekend met de oude sleutel, die de nieuwe sleutel meebrengt.

**Gevolg voor ontwikkeling.** Ontwikkel-OTA's naar je eigen board moeten dan met de releasesleutel worden ondertekend, of met een ontwikkelbuild waarin de ontwikkelsleutel zit. Dat besluit ligt bij de maintainer.

## Publiceren zolang er geen release-workflow is (alleen na toestemming)

1. Bouw en verifieer lokaal: host-tests, alle omgevingen, het bewijspakket en `heltec_v2.sh verify`.
2. Maak de bundels en `SHA256SUMS`. Controleer de hashes.
3. Controleer dat geen workflow reageert op tags of releases.
4. Publiceer: `gh release create vN <bestanden> --notes-file <notes>`, alleen na expliciete toestemming.
