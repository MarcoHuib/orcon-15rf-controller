#!/usr/bin/env bash
# The Matter firmware of a beta release (.github/workflows/release-control.yml), checked as tools/release_ci.py
# verify-build checks the control image, inside the pinned ESP-Matter image of tools/build_idf.sh: its esptool
# regenerates the image from the ELF byte for byte, its toolchain reads the ELF (rollback hook, check_elf_tx.py).
#   tools/release_matter_check.sh <release number> <beta number> <commit>
# Input: idf/build/matter (tools/build_idf.sh --release N --pre P, no identity) and the control release build's board
# files (.pio/build/heltec_v2_sx1276_control: a release never changes the bootloader or the partition table).
# Output: dist/control-matter (BUILD_INFO.json, firmware.bin, the pinned board files). No network in the container.
set -euo pipefail
n=${1:?release number}; pre=${2:?beta number}; commit=${3:?commit}
[[ $n =~ ^[1-9][0-9]{0,8}$ && $pre =~ ^[1-9][0-9]?$ && $commit =~ ^[0-9a-f]{40}$ ]] || { echo "bad arguments" >&2; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
image=$(sed -n 's/^image=\(espressif\/esp-matter@sha256:[0-9a-f]\{64\}\).*/\1/p' "$root/tools/build_idf.sh")
[ -n "$image" ] || { echo "no pinned image in tools/build_idf.sh" >&2; exit 1; }
[ -d "$HOME/.platformio/packages" ] || { echo "no PlatformIO packages (boot_app0.bin of the pinned framework)" >&2; exit 1; }
mkdir -p "$root/dist"
# The container runs as root: its output goes back to the caller (on Linux the next steps write factory images there).
docker run --rm --network none -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -v "$root:/repo" \
    -v "$HOME/.platformio/packages:/root/.platformio/packages:ro" --entrypoint /bin/bash "$image" -c "
  set -euo pipefail
  . \$IDF_PATH/export.sh > /dev/null 2>&1
  cd /repo
  g++ -std=c++17 -O1 -Wall -Wextra -Werror -I src -I .pio/libdeps/heltec_v2_sx1276_control/ArduinoJson/src \
      test/release_accept.cpp src/fw_update.cpp src/ota_policy.cpp -o /tmp/release_accept
  tc=\$(dirname \"\$(command -v xtensa-esp32-elf-nm)\")
  python3 -I tools/release_ci.py verify-build --variant control-matter --build idf/build/matter \
      --boot-files .pio/build/heltec_v2_sx1276_control --version $n --pre $pre --commit $commit \
      --harness /tmp/release_accept --nm \"\$tc/xtensa-esp32-elf-nm\" --out dist/control-matter --esptool module
  chown -R \"\$HOST_UID:\$HOST_GID\" dist
  python3 -I tools/check_elf_tx.py idf/build/matter/orcon_15rf.elf heltec_v2_matter \"\$tc\""
