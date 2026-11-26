#!/usr/bin/env bash
# Build the ESP-IDF 5.5 firmware (idf/, docs/MATTER.md) in the pinned ESP-Matter container.
#   tools/build_idf.sh [--no-matter] [--identity <dir>]
#     --no-matter        Orcon on ESP-IDF 5 without Matter (the toolchain-migration baseline)
#     --identity <dir>   a device's own Matter identity (tools/matter_identity.sh; also $ORCON_MATTER_IDENTITY).
#                        Without one the build uses the placeholder: it compiles, but Matter never starts (CI, checks).
# Output: idf/build/<matter|idf5>/ (ignored by git). An image built WITH an identity holds that device's DAC private
# key and setup code: install it only on that device and never publish or commit it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=espressif/esp-matter@sha256:94ed6a211d5bc6b55c44a36802bdb0e663a92d9aac7ece3944e6a1bec10a7f17   # ESP-IDF 5.5.5, esp-matter v1.6
matter=1
identity=${ORCON_MATTER_IDENTITY:-}
while [ $# -gt 0 ]; do
    case $1 in
        --no-matter) matter=0 ;;
        --identity) identity=${2:?--identity <dir>}; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
commit=$(git -C "$root" rev-parse HEAD)
dirty=$([ -n "$(git -C "$root" status --porcelain -- src idf)" ] && echo "-dirty" || true)
[ -f "$root/idf/components/arduino/CMakeLists.txt" ] || "$root/tools/fetch_idf_deps.sh"
defaults="sdkconfig.defaults;config/memory.defaults"
b=build/idf5
mounts=(-v "$root:/repo")
envs=()
if [ "$matter" = 1 ]; then
    defaults="$defaults;config/clusters.defaults;config/matter.defaults;config/diagnostics.defaults"
    b=build/matter
    if [ -n "$identity" ]; then
        [ -f "$identity/orcon_matter_creds.h" ] || { echo "no identity in $identity (tools/matter_identity.sh)" >&2; exit 2; }
        mounts+=(-v "$(cd "$identity" && pwd):/identity:ro")
        envs+=(-e ORCON_MATTER_IDENTITY=/identity)
        [ -f "$identity/identity.defaults" ] && defaults="$defaults;/identity/identity.defaults"   # its serial number
    else
        echo "build_idf: no Matter identity: placeholder build (compiles; Matter never starts on a board)" >&2
    fi
fi
mkdir -p "$root/idf/$b"
rm -f "$root/idf/$b/sdkconfig"   # sdkconfig.defaults only fill what an existing sdkconfig lacks: always regenerate
docker run --rm "${mounts[@]}" ${envs[@]+"${envs[@]}"} "$image" bash -c "
  set -e
  cd /repo/idf
  idf.py -B $b -D SDKCONFIG=$b/sdkconfig -D 'SDKCONFIG_DEFAULTS=$defaults' -D ORCON_MATTER=$matter \
         -D ORCON_FW_VERSION=$commit -D ORCON_DIRTY=$dirty build
  idf.py -B $b -D SDKCONFIG=$b/sdkconfig size
"
echo "image: idf/$b/orcon_15rf.bin (FW_VERSION $commit$dirty, Matter $matter$([ -n "$identity" ] && echo ", own identity" || true))"
