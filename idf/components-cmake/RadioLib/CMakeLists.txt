# RadioLib 7.8.1 as the ARDUINO variant, exactly as PlatformIO builds it for Orcon.
# (RadioLib's own CMakeLists builds without ARDUINO -> RADIOLIB_BUILD_GENERIC, a different Module
#  API/ABI from the RADIOLIB_BUILD_ARDUINO view that Orcon's sources compile against.)
file(GLOB_RECURSE RADIOLIB_SOURCES "src/*.cpp")
list(FILTER RADIOLIB_SOURCES EXCLUDE REGEX "/src/hal/(ESP-IDF|RPi|RPiPico|Stm32duino)/")
idf_component_register(SRCS ${RADIOLIB_SOURCES} INCLUDE_DIRS "src" REQUIRES arduino)
