# Releasesleutel: scheiding en migratie (ontwerp, nog niet uitgevoerd)

> Status: ontwerp. De sleutel wordt niet gewisseld zonder een apart akkoord van de maintainer. Alleen de beveiliging tegen het per ongeluk publiceren van een privésleutel is al actief (`tools/check_privacy.py`).

## Drie geheimen, elk met één taak

| | Releasesleutel | Eigenaarssleutel | Beheerderswachtwoord |
|---|---|---|---|
| Van wie | uitgever | eigenaar, per Heltec | eigenaar, per Heltec |
| Privé-deel | versleutelde PKCS#8 op twee offline dragers | `orcon-recovery-key.pem` (versleuteld), bij de eigenaar | in het hoofd of de wachtwoordmanager van de eigenaar |
| Op het apparaat | alleen de publieke sleutel, in de firmware | alleen de publieke sleutel, in NVS | alleen een PBKDF2-hash, in NVS |
| Bewijst | "deze firmware is officieel" | "ik ben de eigenaar van dit apparaat" | "ik mag bedienen en updates goedkeuren" |
| Kan niet | toegang geven tot een apparaat, of zonder de sessie van de eigenaar installeren | firmware ondertekenen | firmware echt maken |

**Waar het privé-deel van de releasesleutel nooit mag staan**
- In GitHub: repo, issues, releases.
- In een firmware-image.
- In logs: serial, console, CLI-uitvoer.
- In testfixtures. Tests maken tijdens het draaien wegwerpsleutels (`tools/test_orcon_cli.py`).
- `tools/check_privacy.py` weigert een private-key-blok in elk getrackt bestand, ook als C- of JS-string.

## Huidige situatie

- De firmware vertrouwt één sleutel, K_dev: publiek in `src/ota_pubkey.h`, privé in `~/.orcon/ota-signing-key.pem` op de ontwikkel-Mac.
- Het bestaande board (a5260ad, beide sleuven) vertrouwt alleen K_dev.

## Doel

- **Publieke releasebuilds** vertrouwen alleen {K_rel, K_rel_backup}. Komt de ontwikkel-Mac in verkeerde handen, dan raakt dat geen publiek apparaat.
- **Lokale ontwikkelbuilds** voor je eigen board vertrouwen {K_dev, K_rel, K_rel_backup}. Ontwikkel-OTA's met K_dev blijven dan mogelijk, en officiële releases werken ook.
- Een manifest wordt geverifieerd tegen elke vertrouwde sleutel, maximaal drie keer, zonder sleutel-ID.
- Een flag (bijvoorbeeld `ORCON_TRUST_DEV_KEY`) bepaalt of K_dev erin zit; alleen de ontwikkelomgevingen zetten hem.

## Migratie (elke stap apart akkoord)

1. **Code voorbereiden** (geen sleutelwijziging):
   - de firmware verifieert tegen een lijst vertrouwde sleutels;
   - de flag hierboven;
   - `orcon.py release --key <pad>` tekent met een expliciet opgegeven sleutel en weigert K_dev, een sleutel in een git-werkmap en bestanden die anderen kunnen lezen.

   Bewust nog niet gebouwd: zonder K_rel zou het dode code zijn. Het hoort in dezelfde commit als stap 3.
2. **Sleutelceremonie** door de maintainer, op een offline machine:
   ```
   openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
     | openssl pkcs8 -topk8 -v2 aes-256-cbc -v2prf hmacWithSHA256 -iter 2000000 -out release-key.pem
   openssl pkey -in release-key.pem -pubout          # alleen dit gaat de repo in
   ```
   - Doe hetzelfde voor `release-key-backup.pem`.
   - Bewaar elk privé-bestand op twee versleutelde USB-dragers, op verschillende plaatsen. De wachtwoordzinnen staan in de wachtwoordmanager, nooit op dezelfde drager.
3. **Overgangsfirmware F_t**, getekend met K_dev en vertrouwend {K_dev, K_rel, K_rel_backup}.
   - Installeer via het bestaande pad (`orcon.py ota-install`, getekend met K_dev), in de bekende volgorde: eerst RX-only (app0), dan control (app1). Dan vertrouwt ook de rollback-sleuf K_rel.
   - Mislukt een proefstart, dan valt het board terug op een image dat K_dev vertrouwt. Je kunt het dus altijd opnieuw proberen.
4. **Eerste publieke release R1**: een releasebuild die {K_rel, K_rel_backup} vertrouwt, getekend met `orcon.py release --key /Volumes/<drager>/release-key.pem`.
   - Openssl vraagt de wachtwoordzin. Het bestand wordt nergens heen gekopieerd.
   - Je eigen board accepteert R1 ook.
5. **K_dev uit publieke builds.** Vanaf R1 bevat geen publieke build K_dev. K_dev tekent alleen nog ontwikkelbuilds voor je eigen board.

**Waarom je board updates blijft ontvangen**
- Het vertrouwt nu K_dev.
- F_t is met K_dev getekend en voegt K_rel toe.
- Daarna verifiëren R1 en latere releases met K_rel.
- Op geen enkel moment staat er alleen een image op het board dat een sleutel vertrouwt die je niet hebt.

## Verlies en compromittering

| Gebeurtenis | Gevolg | Herstel |
|---|---|---|
| K_rel kwijt | K_rel_backup tekent | de volgende release vervangt de verloren sleutel |
| K_rel gelekt | een aanvaller kan "officiële" images maken, maar ze niet installeren zonder de sessie van de eigenaar | een release getekend met de backup verwijdert de gelekte sleutel; borden die niet updaten blijven hem vertrouwen (onvermijdelijk) |
| Beide kwijt | geen nieuwe releases meer voor publieke borden | eenmalig opnieuw flashen via USB met een build die nieuwe sleutels vertrouwt |
| K_dev gelekt (Mac) | publieke borden ongemoeid; alleen je eigen ontwikkelboard | ontwikkelsleutel vervangen |

## Wat apart akkoord vraagt

1. De sleutelceremonie (stap 2), door de maintainer zelf.
2. De overgangsfirmware F_t: merge en de twee OTA's naar het bestaande board.
3. De eerste release met K_rel.
