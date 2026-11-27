# Orcon's own sources (../../src), the same files PlatformIO builds. The build flags are those of
# [env:heltec_v2_sx1276_control] in platformio.ini; IDF 5.5 / mbedTLS 3 differences are bridged by the forced-include
# header idf5_compat.h. ORCON_IDF5 / ORCON_MATTER select the few Matter-build-only paths in src/ (config.h).
file(GLOB ORCON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/*.cpp")
idf_component_register(
    SRCS ${ORCON_SOURCES}
    INCLUDE_DIRS "../../src"
    PRIV_INCLUDE_DIRS "."
    REQUIRES arduino RadioLib U8g2 ArduinoJson
    PRIV_REQUIRES espcoredump app_update esp_partition nvs_flash mbedtls esp_wifi esp_timer bootloader_support efuse
                  espressif__network_provisioning   # included by Arduino 3's WiFiGeneric.h / NetworkEvents.h
)
idf_component_optional_requires(PRIVATE orcon_matter)
# Arduino 3: WebServer.h includes Network.h instead of WiFi.h; ota_signed.cpp uses WiFiClient
# (typedef NetworkClient in the WiFi library). Only this file gets the header.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../src/ota_signed.cpp"
    PROPERTIES COMPILE_OPTIONS "-include;WiFiClient.h")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/idf5_compat.h"
    # GCC/IDF -Werror=all turns these into errors; the release workflow builds the same code with
    # -Wno-error=format-truncation (PR #21: cosmetic truncation, no memory risk).
    -Wno-error=format-truncation
    -Wno-error=deprecated-declarations)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CORE_DEBUG_LEVEL=2
    ARDUINOJSON_ENABLE_ARDUINO_STRING=1
    RADIO_SX1276=1
    BOARD_HELTEC_V2=1
    RX_ONLY=1
    ORCON_TX_POC=1
    ORCON_TX_RF_ENABLE=1
    ORCON_WEB_CONTROL=1
    ORCON_TX_PAIR=1
    ORCON_TX_CONTROL=1
    ORCON_IDF5=1
    ORCON_MATTER=${ORCON_MATTER}
    FW_VERSION="${ORCON_FW_VERSION}"   # the 40-hex source commit, see ../CMakeLists.txt
    ORCON_RELEASE_VERSION=${ORCON_RELEASE}   # tools/build_idf.sh --release N (0: a dev build)
    ORCON_RELEASE_PRE=${ORCON_PRE})
