#!/usr/bin/env bash
# Build and run every host test suite. Extra compiler flags via CXXFLAGS, e.g.
#   CXXFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" test/run_host_tests.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$(mktemp -d)"
CXX="${CXX:-g++}"
read -r -a FLAGS <<<"-std=c++17 -O1 -g ${CXXFLAGS:-}"
run() {
    local name=$1; shift
    "$CXX" "${FLAGS[@]}" "$@" -o "$OUT/$name"
    if ! "$OUT/$name" >"$OUT/$name.log" 2>&1; then cat "$OUT/$name.log"; echo "FAILED: $name"; exit 1; fi
    echo "$name: $(tail -1 "$OUT/$name.log")"
}
ORCON="src/orcon.cpp src/orcon_frames.cpp src/orcon_log.cpp src/ramses_codec.cpp"
run codec    -I src test/test_codec.cpp src/ramses_codec.cpp
run frames   -I src test/test_frames.cpp src/ramses_codec.cpp src/orcon_frames.cpp
run rx       -I src test/test_rx.cpp src/rx_stream.cpp src/ramses_codec.cpp
run log      -I src -I test/shim test/test_log.cpp src/ramses_codec.cpp src/orcon_frames.cpp src/orcon_log.cpp
run orcon_rx -DRX_ONLY=1 -I src -I test/shim test/test_orcon.cpp $ORCON
run orcon_v3 -DRX_ONLY=0 -I src -I test/shim test/test_orcon.cpp $ORCON
run rxpipe   -pthread -I src test/test_rxpipe.cpp src/rx_stream.cpp src/ramses_codec.cpp
run netcfg   -I src test/test_netcfg.cpp
