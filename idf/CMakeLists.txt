# The Orcon firmware on ESP-IDF 5.5 with arduino-esp32 3.3.12 as a component, optionally with ESP-Matter (on-network
# commissioning, BLE off): the same sources as the PlatformIO build (../src, never copied), see docs/MATTER.md.
#   tools/build_idf.sh                 Orcon + Matter, in the pinned ESP-Matter container
#   tools/build_idf.sh --no-matter     the same Orcon build without Matter (the toolchain-migration baseline)
# PlatformIO (platformio.ini) stays the build of the official v1.x releases.
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ORCON_MATTER)
    set(ORCON_MATTER 1)
endif()
if(ORCON_MATTER)
    if(NOT DEFINED ENV{ESP_MATTER_PATH})
        set(ENV{ESP_MATTER_PATH} "/opt/espressif/esp-matter")
    endif()
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
endif()
# FW_VERSION: the 40-hex commit of the sources (the field Orcon's OTA image check looks for), passed in by
# tools/build_idf.sh. PROJECT_VER (esp_app_desc, boot log, Matter's SoftwareVersionString) names a development build;
# never a release number (ORCON_RELEASE_VERSION stays 0: the official v1.x releases still come from PlatformIO).
string(LENGTH "${ORCON_FW_VERSION}" _idlen)
if(NOT _idlen EQUAL 40 OR NOT ORCON_FW_VERSION MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "ORCON_FW_VERSION (the 40-hex source commit) is required: build with tools/build_idf.sh")
endif()
string(SUBSTRING "${ORCON_FW_VERSION}" 0 7 _id7)
if(ORCON_MATTER)
    set(PROJECT_VER "matter-dev-${_id7}${ORCON_DIRTY}")
else()
    set(PROJECT_VER "idf5-dev-${_id7}${ORCON_DIRTY}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(ORCON_MATTER)
    include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components")
else()
    list(APPEND EXCLUDE_COMPONENTS orcon_matter)
endif()

project(orcon_15rf)

# Same global flags as the esp-matter examples (release/v1.6 examples/*/CMakeLists.txt).
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
