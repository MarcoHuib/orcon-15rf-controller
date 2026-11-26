#!/usr/bin/env bash
# The libraries of the ESP-IDF build (idf/, docs/MATTER.md), exactly as the PlatformIO release build uses them:
#   - ArduinoJson, RadioLib, U8g2: the lib_deps pins of platformio.ini, copied from what `pio pkg install` resolves for
#     [env:heltec_v2_sx1276_control] (one source for the versions; the same files as the release image)
#   - arduino-esp32 3.3.12 as an ESP-IDF component (git, the tag's commit checked)
# Into idf/components/ (ignored by git), each with our component CMakeLists from idf/components-cmake/.
#   tools/fetch_idf_deps.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
comp=$root/idf/components
ARDUINO_TAG=3.3.12
ARDUINO_COMMIT=94afccf35fb1e401facddbcf9e13bcf7c76a31d8
command -v pio > /dev/null || { echo "PlatformIO (pio) is needed for the pinned libraries: pip install platformio" >&2; exit 2; }
(cd "$root" && pio pkg install -e heltec_v2_sx1276_control > /dev/null)
libs=$root/.pio/libdeps/heltec_v2_sx1276_control
for lib in ArduinoJson RadioLib U8g2; do
    [ -d "$libs/$lib/src" ] || { echo "missing $libs/$lib (pio pkg install failed?)" >&2; exit 1; }
    mkdir -p "$comp/$lib"
    rsync -a --delete --exclude CMakeLists.txt --exclude .piopm "$libs/$lib/" "$comp/$lib/"
    cp "$root/idf/components-cmake/$lib/CMakeLists.txt" "$comp/$lib/CMakeLists.txt"
done
if [ ! -d "$comp/arduino/.git" ]; then
    git clone -q --depth 1 --branch "$ARDUINO_TAG" https://github.com/espressif/arduino-esp32.git "$comp/arduino"
fi
got=$(git -C "$comp/arduino" rev-parse HEAD)
[ "$got" = "$ARDUINO_COMMIT" ] || { echo "arduino-esp32 $ARDUINO_TAG is $got, expected $ARDUINO_COMMIT" >&2; exit 1; }
[ -z "$(git -C "$comp/arduino" status --porcelain)" ] || { echo "idf/components/arduino has local changes" >&2; exit 1; }
echo "idf/components: arduino-esp32 $ARDUINO_TAG ($got); $(grep -E '^ +(jgromes/RadioLib|bblanchon/ArduinoJson|olikraus/U8g2) *@' "$root/platformio.ini" | tr -d ' ' | tr '\n' ' ')"
