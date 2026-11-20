# Uitgangssituatie en A/B-overgang naar alleen control-releases

## Uitgangssituatie (2026-11-19, fase 1)

**Board.** Bron: `~/.orcon/installed.txt` en de projectnotities. Het board is voor dit werk **niet benaderd**.

| Slot | Image | Status |
|---|---|---|
| app1 | b75fad3 control (sha 51a62d52…), via OTA op 19-11 09:07Z | draait, valid |
| app0 | 2edb423 RX-only (sha a5f34920…) | valid, terugvaloptie |
| NVS | WiFi, wachtwoord (legacy, geen herstelsleutel), profiel, RF-ID's, Siri-sleutel | ongewijzigd |

**Wat er al was en hergebruikt wordt** (geen tweede OTA-systeem):
- Partitietabel `default_8MB`: otadata + app0/app1 van 0x330000 B (`tools/check_parts.py`, gepind).
- Bootloader van Arduino-ESP32 2.0.16 met `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (gepind). Een nieuw image start als `PENDING_VERIFY`; een niet-bevestigde start wordt `ABORTED`.
- `ota_signed.cpp`:
  - ECDSA P-256-manifest (v1 voor één board, v2 als release);
  - imagecontrole via `ota_policy` (markers, commitstempel, geen onbeveiligde OTA, eigenaarsmodel);
  - proefstart: bevestigd na ≥ 60 s met WiFi en RX, anders rollback na 300 s;
  - eigenaarssessie + CSRF voor elke installatiestap;
  - `/api/ota/hold` voor de rollbacktest.
- `ota_policy.cpp` heeft al een v2-releasemanifest met `version` (uint32) en downgradeweigering (`older-release`). `orcon.py release` maakt `.orconfw`-bundels. De webpagina had al een handmatige upload.
- Ontbrak: een releaseworkflow, de omzetting van versienummer naar SemVer, het releasepakket in CI, de updatecontrole, de éénknopsinstallatie en een factory-image.

**Compatibiliteitscontracten** (de draaiende b75fad3 moet de eerste release accepteren):

| Contract | Waar | Stand |
|---|---|---|
| Handtekening met `ota_pubkey.h` (SPKI e71543f1…) | `ota_signed.cpp` | ongewijzigd; releases tekenen met dezelfde sleutel |
| v2-manifest `{"v":2,"kind":"orcon-release","env","variant","allow_tx","commit","version","size","sha256"}`, precies 9 velden | `ota_policy.cpp parseManifest` | ongewijzigd; `release_ci.py` gebruikt `orcon.release_manifest` |
| `version ≥ ORCON_RELEASE_VERSION` van het board (b75fad3 = 0) | `checkManifest` | elke release ≥ 1 |
| Image bevat:<br>- `txpoc packet TX (CONTROL: `;<br>- `ORCON-SIGNED-OTA v1`;<br>- `ORCON-OWNER-MODEL v1`;<br>- de commit.<br><br>Geen `SX1276 direct RX (TX disabled)`, `tx-poc-dryrun`, `RF TEST`, `ArduinoOTA`, `Updater.cpp`, `auth_upload` of `Firmware update (OTA)`. | `ota_policy.cpp checkImage` | gecontroleerd in de build (`release_accept`) en met de oude beleidscode van b75fad3 en 2edb423 (zie hieronder) |
| Bundel `.orconfw` = één JSON-regel `{"manifest","sig"}` + image | webpagina, `orcon.py write_bundle` | ongewijzigd |
| Proefstart: `extern "C" verifyRollbackLater()` sterk gelinkt | `check_rollback_hook.py` | gecontroleerd in de releasebuild |
| NVS | — | **geen wijziging**: geen nieuwe of gewijzigde sleutels. Updatestatus staat alleen in RAM. Terugvallen naar b75fad3 leest dezelfde NVS. |

## De overgang (fase 2)

Doel:
- het actieve slot bevat de nieuwste control-release;
- het andere slot bevat de vorige werkende control-release;
- er is geen RX-only-image meer als standaardterugvaloptie.

| Stap | app0 | app1 | draait |
|---|---|---|---|
| nu | 2edb423 RX-only (valid) | b75fad3 control (valid) | app1 |
| 1e release v1.0.0 **geslaagd** | v1.0.0 control (valid) | b75fad3 control (valid, terugval) | app0 |
| 1e release **ongezond** of **reset in proefstart** | v1.0.0 (invalid/aborted) | b75fad3 (valid) | app1 |
| stroom weg **tijdens** download/schrijven | geen image meer (RX-only al gewist) | b75fad3 (valid) | app1 |
| stroom weg **na** het kiezen van het opstartslot | v1.0.0 (new → proefstart) | b75fad3 | app0, in proefstart |
| 2e release v1.1.0 geslaagd | v1.0.0 (valid, terugval) | v1.1.0 (valid) | app1 |
| v1.1.0 ongezond | v1.0.0 (valid) | v1.1.0 (invalid) | app0 |

Bron: `tools/test_ab_transition.py`. Die test schrijft echte otadata-bytes volgens ESP-IDF 4.4 (`esp_ota_set_boot_partition`, valid en invalid markeren, de bootloaderstap met rollback) en laat `tools/check_slots.otadata()` beslissen welk slot start, voor elke pariteit van het volgnummer.

**Waarom dit veilig is:**
- Een update gaat altijd naar het slot dat **niet** draait (`esp_ota_get_next_update_partition`). De draaiende b75fad3 wordt bij de eerste release dus nooit overschreven.
- Het opstartslot verandert pas als het volledige image alle controles heeft doorstaan. Tot dan blijft b75fad3 het opstartimage.
- Tijdens een proefstart wordt geen nieuwe update gestart, zowel in `ota_signed.cpp` als in `fw_update_esp.cpp`.
- Een afgewezen image wordt onbootbaar gemaakt (eerste sector gewist). Een proefstart die zich niet kan bevestigen, valt na 300 s terug (Codex r3).

**Wat je verliest, en dat is bewust (opdracht fase 2):** zodra de eerste control-release begint te schrijven, is het RX-only-image in app0 weg, **ook als de update daarna mislukt**. b75fad3 control blijft altijd de werkende terugval.

Mislukt de eerste update, dan heeft het board even maar één geldig image. De volgende geslaagde update herstelt de A/B-situatie. Wil je die tijdelijke situatie vermijden, dan is er alleen de USB-herstelroute (`heltec_v2.sh recover`, apart akkoord).

**Bootloader, partitietabel en herstel blijven gelijk.** Een release schrijft nooit buiten het inactieve app-slot en otadata. De releasebuild controleert dat bootloader, partitietabel en boot_app0 byte-gelijk zijn aan de gepinde bestanden. `heltec_v2.sh recover` blijft werken, want de releases worden in `installed.txt` herkend zodra je ze via `orcon.py` installeert. Na een installatie via de webpagina voeg je de SHA-256 uit `SHA256SUMS` toe aan `~/.orcon/installed.txt`.

## Acceptatie door de oude beleidscode

De releasebundel van deze branch is met `release_accept` gebouwd tegen de `ota_policy.cpp` van **b75fad3** en van **2edb423**. Dat is de code die nu op het board draait. Gecontroleerd, met een lokale releasebuild met wegwerpsleutel; zie het testbewijs in de PR:
- v2-manifest geaccepteerd;
- image als `control` geaccepteerd;
- een controller op dezelfde of een hogere versie weigert hem (`older-release`).

## Wat alleen op hardware kan

- De echte bootloaderovergang.
- WiFi en RX tijdens de proefstart.
- Stroomuitval.
- Het TLS-geheugen tijdens download en schrijven.

Checklist in [RELEASES.md](RELEASES.md#hardwarevalidatie-nog-te-doen-met-toestemming-per-stap).
