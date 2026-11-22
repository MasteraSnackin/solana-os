include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

# 44VBA (vba-next) is C++ and ESP-IDF-only. The web emulator project is C and
# does not compile these; gbaemu_app.c stubs the core there.
set(SOLANA_OS_GBA_SRCS
    apps/gbaemu/gbaemu_host.cpp
    apps/gbaemu/vendor/44vba/gba.cpp
    apps/gbaemu/vendor/44vba/memory.cpp
    apps/gbaemu/vendor/44vba/sound.cpp
)

idf_component_register(
    SRCS ${SOLANA_OS_SRCS} ${SOLANA_OS_DEVICE_SRCS} ${SOLANA_OS_GBA_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event console esp_timer mbedtls
                  esp_app_format app_update esp_partition spi_flash fatfs vfs esp_http_client esp-tls tcp_transport lwip esp_driver_uart bt)

set_property(SOURCE ${SOLANA_OS_GBA_SRCS} APPEND PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/apps/gbaemu
    ${CMAKE_CURRENT_LIST_DIR}/apps/gbaemu/vendor/44vba)
set_property(SOURCE ${SOLANA_OS_GBA_SRCS} APPEND PROPERTY COMPILE_OPTIONS
    -O3 -fno-exceptions -fno-rtti -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-sign-compare -Wno-implicit-fallthrough -Wno-maybe-uninitialized
    -Wno-type-limits -Wno-missing-field-initializers -Wno-format
    -Wno-deprecated-copy
    -DFRONTEND_SUPPORTS_RGB565 -DHAVE_STDINT_H -DINLINE=inline -DLSB_FIRST
    -DUSE_FRAME_SKIP -DUSE_TWEAKS -DGBA_MUTE_AUDIO)

# Peanut-GB CPU decode/execute and LCD line: -O3 in IRAM. gbemu_app.c
# keeps the project's default -Os flags (picker, blit helper, save).
set_property(SOURCE apps/gbemu/peanut_gb_cpu.c APPEND PROPERTY COMPILE_OPTIONS
    -O3 -DPEANUT_GB_CPU_IRAM -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough)

# cJSON (espressif/cjson) parses mesh JSON with a depth cap of 16:
# CONFIG_CJSON_NESTING_LIMIT in sdkconfig.defaults.
