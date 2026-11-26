set(srcs "orcon_matter.cpp" "orcon_matter_mdns_port.cpp" "orcon_matter_nvs.cpp" "arduino_matter_wraps.cpp" "orcon_matter_memweb.cpp"
         "orcon_matter_creds.cpp" "orcon_matter_identity.cpp" "orcon_matter_pairing.cpp")
if(CONFIG_ORCON_MATTER_HEAPMON)
    list(APPEND srcs "orcon_matter_heapmon.cpp")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "."
                       REQUIRES esp_matter arduino
                       PRIV_REQUIRES lwip espressif__network_provisioning nvs_flash esp_partition esp_event esp_netif esp_timer esp_wifi mbedtls ArduinoJson)   # WiFi.h -> WiFiGeneric.h needs network_provisioning
# The Matter identity lives on the device (orcon_matter_identity.h): the universal firmware is built with placeholder/
# (no identity in the image) and makes one on the device when the owner activates Matter. Only a private migration build
# embeds an existing identity (tools/build_idf.sh --identity, named in ORCON_MATTER_IDENTITY, kept OUTSIDE the
# repository): it moves that identity onto the device once. Never commit an identity or an image built with one.
if(DEFINED ENV{ORCON_MATTER_IDENTITY} AND EXISTS "$ENV{ORCON_MATTER_IDENTITY}/orcon_matter_creds.h")
    target_include_directories(${COMPONENT_LIB} PRIVATE "$ENV{ORCON_MATTER_IDENTITY}")
else()
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/placeholder")
    message(STATUS "orcon_matter: universal firmware (no identity in the image; made on the device at activation)")
endif()
# Without editing Orcon's sources (see orcon_matter.cpp): the telemetry task starts with Orcon's setup(), Matter
# follows Orcon's net::begin(), or net::loop() once the home network is up after a late router.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=_Z5setupv" "-Wl,--wrap=_ZN3net5beginEv"
                      "-Wl,--wrap=_ZN3net4loopEv")
# ota::begin(WebServer&) also registers the read-only GET /api/mem (orcon_matter_memweb.cpp) and the owner's Matter
# routes (/connect). A trial boot confirms only with Matter running (orcon_matter.cpp).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ota_mark_app_valid_cancel_rollback")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=_ZN3ota5beginER9WebServer")
if(CONFIG_ORCON_MATTER_HEAPMON)
    # Heap monitor: mark RF transmissions (radio::sx1276::txBegin, called from tx_poc_runtime.cpp), register its
    # Wi-Fi/IP handlers the moment the default event loop exists (before Wi-Fi starts), mark the start of Orcon's
    # update job (its "fw_update" task) and keep the result of Arduino's nvs_flash_init() before setup().
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=_ZN5radio6sx12767txBeginEPKhj"
                          "-Wl,--wrap=esp_event_loop_create_default" "-Wl,--wrap=xTaskCreatePinnedToCore"
                          "-Wl,--wrap=nvs_flash_init")
endif()
# UDP 5353 shared by Orcon's mDNS and Matter's (orcon_matter_mdns_port.cpp).
if(CONFIG_ORCON_MATTER_SHARE_MDNS_PORT)
    # -u: nothing else needs this object, so without it the linker never pulls it from the component library
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=udp_bind" "-Wl,-u,__wrap_udp_bind")
endif()
