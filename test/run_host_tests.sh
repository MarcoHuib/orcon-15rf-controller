#!/usr/bin/env bash
# Build and run every host test suite. Extra compiler flags via CXXFLAGS, e.g.
#   CXXFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" test/run_host_tests.sh
# Each binary has a timeout (HOST_TEST_TIMEOUT_SECONDS, default 60), including
# runtime initialization before main(), so a broken sanitizer cannot hang forever.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${HOST_TEST_OUTPUT_DIR:-$(mktemp -d)}"
mkdir -p "$OUT"
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
run tx_poc   -pthread -Wall -Wextra -Werror -I src test/test_tx_poc.cpp src/tx_poc.cpp src/ramses_codec.cpp src/orcon_frames.cpp
run tx_budget -Wall -Wextra -Werror -I src test/test_tx_budget.cpp src/tx_budget.cpp
run profiles -Wall -Wextra -Werror -I src test/test_remote_profiles.cpp src/remote_profiles.cpp src/setup_flow.cpp
run setup    -Wall -Wextra -Werror -I src test/test_setup.cpp src/setup_flow.cpp
run rf_console -Wall -Wextra -Werror -I src test/test_rf_console.cpp src/rf_console.cpp src/ramses_codec.cpp
# Setup wizard HTTP glue: the real setup_web.cpp behind a fake WebServer, RX-only, TX and
# web-control (txpoc layer). Needs the pinned ArduinoJson that `pio run -e heltec_v2_sx1276` fetches.
AJ=.pio/libdeps/heltec_v2_sx1276/ArduinoJson/src
run ota_policy -Wall -Wextra -Werror -I src -I $AJ test/test_ota_policy.cpp src/ota_policy.cpp
run web_auth -Wall -Wextra -Werror -I src -I $AJ test/test_web_auth.cpp src/web_auth.cpp
run provision -Wall -Wextra -Werror -I src test/test_provision.cpp src/provision.cpp
run reset_gesture -Wall -Wextra -Werror -I src test/test_reset_gesture.cpp
# The owner key file against OpenSSL (primitives and the openssl CLI as the independent reader).
OSSL="$( (brew --prefix openssl 2>/dev/null || true) | head -1)"
if [ -n "$OSSL" ] && [ -f "$OSSL/include/openssl/evp.h" ]; then
    run owner_key -Wall -Wextra -Werror -I src -I "$OSSL/include" test/test_owner_key.cpp src/owner_key.cpp -L "$OSSL/lib" -lcrypto
elif pkg-config --exists libcrypto 2>/dev/null; then
    run owner_key -Wall -Wextra -Werror -I src $(pkg-config --cflags libcrypto) test/test_owner_key.cpp src/owner_key.cpp $(pkg-config --libs libcrypto)
else
    echo "owner_key: NOT TESTED (no OpenSSL headers/libcrypto found)"
fi
WEB="-Wall -Wextra -Werror -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -I test/shim_web -I src -I $AJ test/test_setup_web.cpp src/setup_web.cpp src/setup_flow.cpp src/remote_profiles.cpp src/rf_console.cpp src/web_auth.cpp src/owner_key.cpp src/provision.cpp $ORCON"
if [ -d "$AJ" ]; then
    run setup_web_rx -DRADIO_SX1276=1 -DRX_ONLY=1 $WEB
    run setup_web_rx_v2 -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 $WEB          # RX-only rollback image: web login
    run setup_web_tx -DRADIO_CC1101_ASYNC=1 -DRX_ONLY=0 $WEB
    run setup_web_webctl -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 $WEB src/tx_poc.cpp
    run setup_web_webctl_rf -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 -DORCON_TX_RF_ENABLE=1 $WEB src/tx_poc.cpp
    run setup_web_rftest -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 -DORCON_TX_RF_ENABLE=1 -DORCON_TX_MAX_PER_BOOT=50 -DORCON_TX_PAIR=1 $WEB src/tx_poc.cpp
    run setup_web_control -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 -DORCON_TX_RF_ENABLE=1 -DORCON_TX_PAIR=1 -DORCON_TX_CONTROL=1 $WEB src/tx_poc.cpp
    # A new device end to end (wizard, key, recovery, refusals): the control image and the RX-only rollback image.
    run setup_web_provision -DPROVISION_SCENARIO=1 -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 -DORCON_TX_RF_ENABLE=1 -DORCON_TX_PAIR=1 -DORCON_TX_CONTROL=1 $WEB src/tx_poc.cpp
    run setup_web_provision_rx -DPROVISION_SCENARIO=1 -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 $WEB
    run setup_web_provision_ambiguous -DPROVISION_SCENARIO=3 -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 -DORCON_TX_RF_ENABLE=1 -DORCON_TX_PAIR=1 -DORCON_TX_CONTROL=1 $WEB src/tx_poc.cpp
    run setup_web_provision_unreadable -DPROVISION_SCENARIO=2 -DRADIO_SX1276=1 -DRX_ONLY=1 -DBOARD_HELTEC_V2=1 -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 -DORCON_TX_RF_ENABLE=1 -DORCON_TX_PAIR=1 -DORCON_TX_CONTROL=1 $WEB src/tx_poc.cpp
else
    echo "setup_web: NOT TESTED (no $AJ; run 'pio run -e heltec_v2_sx1276' once)"
fi
python3 tools/test_sx1276_tx.py
python3 tools/gen_setup_page.py --check
node test/test_setup_page.js | tail -1
