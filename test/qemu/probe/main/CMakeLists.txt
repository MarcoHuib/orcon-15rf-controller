idf_component_register(SRCS "probe.cpp" INCLUDE_DIRS "."
                       REQUIRES arduino esp_matter orcon_matter
                       PRIV_REQUIRES esp_eth esp_netif nvs_flash app_update espressif__network_provisioning)
