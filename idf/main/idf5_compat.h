// Forced include (-include) for the unmodified Orcon v1.0.1 sources when they are built on
// ESP-IDF 5.x / mbedTLS 3 instead of PlatformIO's Arduino 2.0.16 (IDF 4.4 / mbedTLS 2).
// Only names change here: same functions, same semantics, same struct layouts.
#pragma once
#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif

#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
// owner_store.cpp passes &grp.G to mbedtls_ecp_mul(); mbedTLS 3 renames private struct members
// (MBEDTLS_PRIVATE) unless this is defined before the first mbedTLS header. Layout is unchanged.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS 1
// esp_read_mac()/ESP_MAC_WIFI_STA moved from esp_system.h to esp_mac.h in IDF 5.0.
#include <esp_mac.h>
// SPI_FLASH_SEC_SIZE: esp_spi_flash.h (reached via esp_ota_ops.h in IDF 4.4) became spi_flash_mmap.h.
#include <spi_flash_mmap.h>
// (Arduino 3's WebServer.h no longer brings in WiFiClient: main/CMakeLists.txt adds
//  -include WiFiClient.h for ota_signed.cpp only, so Arduino.h macros stay out of the other files.)
// mbedTLS 3.0 dropped the _ret suffix; the suffix-less functions have the same signature and
// return value (int, 0 = success) as the mbedTLS 2.x *_ret ones Orcon calls.
#define mbedtls_sha256_ret        mbedtls_sha256
#define mbedtls_sha256_starts_ret mbedtls_sha256_starts
#define mbedtls_sha256_update_ret mbedtls_sha256_update
#define mbedtls_sha256_finish_ret mbedtls_sha256_finish

// auth_store.cpp derives the password hash with the mbedTLS 2 context API. IDF 5.5 builds
// mbedTLS 3.6 with MBEDTLS_DEPRECATED_REMOVED, which drops that entry point; the _ext function
// computes the same PBKDF2-HMAC for the context's digest (auth_store's boot self-test with the
// published PBKDF2 test vector checks the result on the device).
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#if defined(MBEDTLS_DEPRECATED_REMOVED)
static inline int mbedtls_pkcs5_pbkdf2_hmac(mbedtls_md_context_t* ctx, const unsigned char* password, size_t plen,
                                            const unsigned char* salt, size_t slen, unsigned int iteration_count,
                                            uint32_t key_length, unsigned char* output) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_ctx(ctx);
    if (!info) return MBEDTLS_ERR_PKCS5_BAD_INPUT_DATA;
    return mbedtls_pkcs5_pbkdf2_hmac_ext(mbedtls_md_get_type(info), password, plen, salt, slen, iteration_count,
                                         key_length, output);
}
#endif
#endif
