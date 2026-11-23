include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

# 44VBA (vba-next) is C++ and ESP-IDF-only. The web emulator project is C and
# does not compile these; gbaemu_app.c stubs the core there.
set(SOLANA_OS_GBA_SRCS
    apps/gbaemu/gbaemu_host.cpp
    apps/gbaemu/vendor/44vba/gba.cpp
    apps/gbaemu/vendor/44vba/memory.cpp
    apps/gbaemu/vendor/44vba/sound.cpp
)

# snes9x2002 C interpreter (PocketSNES). ESP-IDF only; snesemu_app.c stubs
# the core on the web emulator. No ARM asm / ASMCPU / ASM_SPC700.
set(SOLANA_OS_SNES_VENDOR ${CMAKE_CURRENT_LIST_DIR}/apps/snesemu/vendor/snes9x2002)
set(SOLANA_OS_SNES_SRCS
    apps/snesemu/snesemu_host.c
    ${SOLANA_OS_SNES_VENDOR}/libretro-common/streams/memory_stream.c
    ${SOLANA_OS_SNES_VENDOR}/src/apu.c
    ${SOLANA_OS_SNES_VENDOR}/src/apuaux.c
    ${SOLANA_OS_SNES_VENDOR}/src/c4.c
    ${SOLANA_OS_SNES_VENDOR}/src/c4emu.c
    ${SOLANA_OS_SNES_VENDOR}/src/clip.c
    ${SOLANA_OS_SNES_VENDOR}/src/data.c
    ${SOLANA_OS_SNES_VENDOR}/src/dsp1.c
    ${SOLANA_OS_SNES_VENDOR}/src/fxemu.c
    ${SOLANA_OS_SNES_VENDOR}/src/fxinst.c
    ${SOLANA_OS_SNES_VENDOR}/src/globals.c
    ${SOLANA_OS_SNES_VENDOR}/src/dma.c
    ${SOLANA_OS_SNES_VENDOR}/src/memmap.c
    ${SOLANA_OS_SNES_VENDOR}/src/cpu.c
    ${SOLANA_OS_SNES_VENDOR}/src/cpuexec.c
    ${SOLANA_OS_SNES_VENDOR}/src/cpuops.c
    ${SOLANA_OS_SNES_VENDOR}/src/sa1.c
    ${SOLANA_OS_SNES_VENDOR}/src/sa1cpu.c
    ${SOLANA_OS_SNES_VENDOR}/src/sdd1.c
    ${SOLANA_OS_SNES_VENDOR}/src/sdd1emu.c
    ${SOLANA_OS_SNES_VENDOR}/src/soundux.c
    ${SOLANA_OS_SNES_VENDOR}/src/spc700.c
    ${SOLANA_OS_SNES_VENDOR}/src/srtc.c
    ${SOLANA_OS_SNES_VENDOR}/src/ppu_.c
    ${SOLANA_OS_SNES_VENDOR}/src/gfx.c
    ${SOLANA_OS_SNES_VENDOR}/src/tile.c
)

idf_component_register(
    SRCS ${SOLANA_OS_SRCS} ${SOLANA_OS_DEVICE_SRCS} ${SOLANA_OS_GBA_SRCS} ${SOLANA_OS_SNES_SRCS}
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

set_property(SOURCE ${SOLANA_OS_SNES_SRCS} APPEND PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/apps/snesemu
    ${SOLANA_OS_SNES_VENDOR}/src
    ${SOLANA_OS_SNES_VENDOR}/libretro-common/include)
set_property(SOURCE ${SOLANA_OS_SNES_SRCS} APPEND PROPERTY COMPILE_OPTIONS
    -O3 -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-sign-compare -Wno-implicit-fallthrough -Wno-maybe-uninitialized
    -Wno-type-limits -Wno-missing-field-initializers -Wno-format
    -Wno-pointer-sign -Wno-strict-aliasing -Wno-old-style-declaration
    -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H
    -DUSE_SA1 -D__OLD_RASTER_FX__ -DRIGHTSHIFT_IS_SAR)

# Walnut-CGB CPU/PPU: -O3 + IRAM on 8-bit dispatch / draw_line (see
# WALNUT_GB_CPU_IRAM). Dual-fetch interpreter stays in flash. gbemu_app.c
# stays -Os (picker, blit, save).
set_property(SOURCE apps/gbemu/walnut_cgb_cpu.c APPEND PROPERTY COMPILE_OPTIONS
    -O3 -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough -Wno-type-limits)

# cJSON (espressif/cjson) parses mesh JSON with a depth cap of 16:
# CONFIG_CJSON_NESTING_LIMIT in sdkconfig.defaults.
