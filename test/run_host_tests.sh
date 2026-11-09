#!/usr/bin/env bash
# Build and run every host test suite. Extra compiler flags via CXXFLAGS, e.g.
#   CXXFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" test/run_host_tests.sh
# Each binary has a timeout (HOST_TEST_TIMEOUT_SECONDS, default 60), including
# runtime initialization before main(), so a broken sanitizer cannot hang forever.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="$(mktemp -d)"
CXX="${CXX:-g++}"
read -r -a FLAGS <<<"-std=c++17 -O1 -g ${CXXFLAGS:-}"
run() {
    local name=$1; shift
    "$CXX" "${FLAGS[@]}" "$@" -o "$OUT/$name"
    if ! python3 - "$OUT/$name" "$OUT/$name.log" "${HOST_TEST_TIMEOUT_SECONDS:-60}" <<'PY'
import subprocess, sys
binary, log, timeout = sys.argv[1:]
with open(log, "w") as output:
    try:
        result = subprocess.run([binary], stdout=output, stderr=subprocess.STDOUT,
                                timeout=float(timeout))
    except subprocess.TimeoutExpired:
        output.write(f"TIMEOUT after {timeout}s: {binary} (possibly before main in the sanitizer runtime)\n")
        sys.exit(124)
sys.exit(result.returncode)
PY
    then cat "$OUT/$name.log"; echo "FAILED: $name"; exit 1; fi
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
run oled     -I src test/test_oled_power.cpp
