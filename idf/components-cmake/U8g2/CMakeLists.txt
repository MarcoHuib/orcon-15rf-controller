file(GLOB U8G2_C_SOURCES "src/clib/*.c")
file(GLOB U8G2_CPP_SOURCES "src/*.cpp")
idf_component_register(SRCS ${U8G2_C_SOURCES} ${U8G2_CPP_SOURCES} INCLUDE_DIRS "src" "src/clib" REQUIRES arduino)
