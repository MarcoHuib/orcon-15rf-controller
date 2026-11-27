# QEMU memory and behaviour test of the Matter component (tools/qemu_memtest.sh, docs/MATTER.md): the real
# idf/components/orcon_matter with the real CHIP clusters, Matter over QEMU's emulated Ethernet, and Orcon's admission
# and unit simulated in main/probe.cpp. Built with the firmware's own configuration layers plus qemu.defaults.
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    set(ENV{ESP_MATTER_PATH} "/opt/espressif/esp-matter")
endif()
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
set(PROJECT_VER "qemu-memtest")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../idf/components"   # orcon_matter and the fetched libraries (tools/fetch_idf_deps.sh)
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components")
project(orcon_qemu_memtest)

# The same global flags as the firmware (idf/CMakeLists.txt) and the esp-matter examples.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
