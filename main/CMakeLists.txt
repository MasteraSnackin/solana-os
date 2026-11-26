include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

# 44VBA (vba-next) is C++ and ESP-IDF-only. The web emulator project is C and
# does not compile these; gbaemu_app.c stubs the core there.
set(SOLANA_OS_GBA_SRCS
    apps/gbaemu/gbaemu_host.cpp
    apps/gbaemu/vendor/44vba/gba.cpp
    apps/gbaemu/vendor/44vba/memory.cpp
    apps/gbaemu/vendor/44vba/sound.cpp
)

# Nofrendo NES core (Matthew Conte; Espressif esp32-nesemu port). ESP-IDF
# only; nesemu_app.c stubs the core on the web emulator. Sound muted.
set(SOLANA_OS_NES_VENDOR ${CMAKE_CURRENT_LIST_DIR}/apps/nesemu/vendor/nofrendo)
set(SOLANA_OS_NES_SRCS
    apps/nesemu/nesemu_host.c
    ${SOLANA_OS_NES_VENDOR}/bitmap.c
    ${SOLANA_OS_NES_VENDOR}/config.c
    ${SOLANA_OS_NES_VENDOR}/cpu/nes6502.c
    ${SOLANA_OS_NES_VENDOR}/event.c
    ${SOLANA_OS_NES_VENDOR}/gui.c
    ${SOLANA_OS_NES_VENDOR}/gui_elem.c
    ${SOLANA_OS_NES_VENDOR}/intro.c
    ${SOLANA_OS_NES_VENDOR}/libsnss/libsnss.c
    ${SOLANA_OS_NES_VENDOR}/log.c
    ${SOLANA_OS_NES_VENDOR}/memguard.c
    ${SOLANA_OS_NES_VENDOR}/pcx.c
    ${SOLANA_OS_NES_VENDOR}/vid_drv.c
    ${SOLANA_OS_NES_VENDOR}/nes/mmclist.c
    ${SOLANA_OS_NES_VENDOR}/nes/nes.c
    ${SOLANA_OS_NES_VENDOR}/nes/nes_mmc.c
    ${SOLANA_OS_NES_VENDOR}/nes/nes_pal.c
    ${SOLANA_OS_NES_VENDOR}/nes/nes_ppu.c
    ${SOLANA_OS_NES_VENDOR}/nes/nes_rom.c
    ${SOLANA_OS_NES_VENDOR}/nes/nesinput.c
    ${SOLANA_OS_NES_VENDOR}/nes/nesstate.c
    ${SOLANA_OS_NES_VENDOR}/sndhrdw/fds_snd.c
    ${SOLANA_OS_NES_VENDOR}/sndhrdw/mmc5_snd.c
    ${SOLANA_OS_NES_VENDOR}/sndhrdw/nes_apu.c
    ${SOLANA_OS_NES_VENDOR}/sndhrdw/vrcvisnd.c
)
foreach(_map 000 001 002 003 004 005 007 008 009 011 015 016 018 019 024 032 033 034 040 041 042 046 050 064 065 066 070 073 075 078 079 085 087 093 094 099 160 229 231)
    list(APPEND SOLANA_OS_NES_SRCS ${SOLANA_OS_NES_VENDOR}/mappers/map${_map}.c)
endforeach()
list(APPEND SOLANA_OS_NES_SRCS ${SOLANA_OS_NES_VENDOR}/mappers/mapvrc.c)

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

# doomgeneric (Chocolate Doom / ESP-DOOM). ESP-IDF only; doom_app.c stubs
# the core on the web emulator. Sound muted (FEATURE_SOUND undefined).
set(SOLANA_OS_DOOM_VENDOR ${CMAKE_CURRENT_LIST_DIR}/apps/doom/vendor/doomgeneric)
set(SOLANA_OS_DOOM_SRCS
    apps/doom/doom_host.c
    apps/doom/doomgeneric_solana.c
    ${SOLANA_OS_DOOM_VENDOR}/am_map.c
    ${SOLANA_OS_DOOM_VENDOR}/d_event.c
    ${SOLANA_OS_DOOM_VENDOR}/d_items.c
    ${SOLANA_OS_DOOM_VENDOR}/d_iwad.c
    ${SOLANA_OS_DOOM_VENDOR}/d_loop.c
    ${SOLANA_OS_DOOM_VENDOR}/d_main.c
    ${SOLANA_OS_DOOM_VENDOR}/d_mode.c
    ${SOLANA_OS_DOOM_VENDOR}/d_net.c
    ${SOLANA_OS_DOOM_VENDOR}/doomdef.c
    ${SOLANA_OS_DOOM_VENDOR}/doomgeneric.c
    ${SOLANA_OS_DOOM_VENDOR}/doomstat.c
    ${SOLANA_OS_DOOM_VENDOR}/dstrings.c
    ${SOLANA_OS_DOOM_VENDOR}/dummy.c
    ${SOLANA_OS_DOOM_VENDOR}/f_finale.c
    ${SOLANA_OS_DOOM_VENDOR}/f_wipe.c
    ${SOLANA_OS_DOOM_VENDOR}/g_game.c
    ${SOLANA_OS_DOOM_VENDOR}/gusconf.c
    ${SOLANA_OS_DOOM_VENDOR}/hu_lib.c
    ${SOLANA_OS_DOOM_VENDOR}/hu_stuff.c
    ${SOLANA_OS_DOOM_VENDOR}/i_cdmus.c
    ${SOLANA_OS_DOOM_VENDOR}/i_endoom.c
    ${SOLANA_OS_DOOM_VENDOR}/i_input.c
    ${SOLANA_OS_DOOM_VENDOR}/i_joystick.c
    ${SOLANA_OS_DOOM_VENDOR}/i_scale.c
    ${SOLANA_OS_DOOM_VENDOR}/i_sound.c
    ${SOLANA_OS_DOOM_VENDOR}/i_system.c
    ${SOLANA_OS_DOOM_VENDOR}/i_timer.c
    ${SOLANA_OS_DOOM_VENDOR}/i_video.c
    ${SOLANA_OS_DOOM_VENDOR}/icon.c
    ${SOLANA_OS_DOOM_VENDOR}/info.c
    ${SOLANA_OS_DOOM_VENDOR}/m_argv.c
    ${SOLANA_OS_DOOM_VENDOR}/m_bbox.c
    ${SOLANA_OS_DOOM_VENDOR}/m_cheat.c
    ${SOLANA_OS_DOOM_VENDOR}/m_config.c
    ${SOLANA_OS_DOOM_VENDOR}/m_controls.c
    ${SOLANA_OS_DOOM_VENDOR}/m_fixed.c
    ${SOLANA_OS_DOOM_VENDOR}/m_menu.c
    ${SOLANA_OS_DOOM_VENDOR}/m_misc.c
    ${SOLANA_OS_DOOM_VENDOR}/m_random.c
    ${SOLANA_OS_DOOM_VENDOR}/memio.c
    ${SOLANA_OS_DOOM_VENDOR}/mus2mid.c
    ${SOLANA_OS_DOOM_VENDOR}/p_ceilng.c
    ${SOLANA_OS_DOOM_VENDOR}/p_doors.c
    ${SOLANA_OS_DOOM_VENDOR}/p_enemy.c
    ${SOLANA_OS_DOOM_VENDOR}/p_floor.c
    ${SOLANA_OS_DOOM_VENDOR}/p_inter.c
    ${SOLANA_OS_DOOM_VENDOR}/p_lights.c
    ${SOLANA_OS_DOOM_VENDOR}/p_map.c
    ${SOLANA_OS_DOOM_VENDOR}/p_maputl.c
    ${SOLANA_OS_DOOM_VENDOR}/p_mobj.c
    ${SOLANA_OS_DOOM_VENDOR}/p_plats.c
    ${SOLANA_OS_DOOM_VENDOR}/p_pspr.c
    ${SOLANA_OS_DOOM_VENDOR}/p_saveg.c
    ${SOLANA_OS_DOOM_VENDOR}/p_setup.c
    ${SOLANA_OS_DOOM_VENDOR}/p_sight.c
    ${SOLANA_OS_DOOM_VENDOR}/p_spec.c
    ${SOLANA_OS_DOOM_VENDOR}/p_switch.c
    ${SOLANA_OS_DOOM_VENDOR}/p_telept.c
    ${SOLANA_OS_DOOM_VENDOR}/p_tick.c
    ${SOLANA_OS_DOOM_VENDOR}/p_user.c
    ${SOLANA_OS_DOOM_VENDOR}/r_bsp.c
    ${SOLANA_OS_DOOM_VENDOR}/r_data.c
    ${SOLANA_OS_DOOM_VENDOR}/r_draw.c
    ${SOLANA_OS_DOOM_VENDOR}/r_main.c
    ${SOLANA_OS_DOOM_VENDOR}/r_plane.c
    ${SOLANA_OS_DOOM_VENDOR}/r_segs.c
    ${SOLANA_OS_DOOM_VENDOR}/r_sky.c
    ${SOLANA_OS_DOOM_VENDOR}/r_things.c
    ${SOLANA_OS_DOOM_VENDOR}/s_sound.c
    ${SOLANA_OS_DOOM_VENDOR}/sha1.c
    ${SOLANA_OS_DOOM_VENDOR}/sounds.c
    ${SOLANA_OS_DOOM_VENDOR}/st_lib.c
    ${SOLANA_OS_DOOM_VENDOR}/st_stuff.c
    ${SOLANA_OS_DOOM_VENDOR}/statdump.c
    ${SOLANA_OS_DOOM_VENDOR}/tables.c
    ${SOLANA_OS_DOOM_VENDOR}/v_video.c
    ${SOLANA_OS_DOOM_VENDOR}/w_checksum.c
    ${SOLANA_OS_DOOM_VENDOR}/w_file.c
    ${SOLANA_OS_DOOM_VENDOR}/w_file_stdc.c
    ${SOLANA_OS_DOOM_VENDOR}/w_main.c
    ${SOLANA_OS_DOOM_VENDOR}/w_wad.c
    ${SOLANA_OS_DOOM_VENDOR}/wi_stuff.c
    ${SOLANA_OS_DOOM_VENDOR}/z_zone.c
)

idf_component_register(
    SRCS ${SOLANA_OS_SRCS} ${SOLANA_OS_DEVICE_SRCS} ${SOLANA_OS_GBA_SRCS} ${SOLANA_OS_SNES_SRCS} ${SOLANA_OS_NES_SRCS} ${SOLANA_OS_DOOM_SRCS}
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

set_property(SOURCE ${SOLANA_OS_NES_SRCS} APPEND PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/apps/nesemu
    ${SOLANA_OS_NES_VENDOR}
    ${SOLANA_OS_NES_VENDOR}/cpu
    ${SOLANA_OS_NES_VENDOR}/nes
    ${SOLANA_OS_NES_VENDOR}/sndhrdw
    ${SOLANA_OS_NES_VENDOR}/libsnss
    ${SOLANA_OS_NES_VENDOR}/mappers)
set_property(SOURCE ${SOLANA_OS_NES_SRCS} APPEND PROPERTY COMPILE_OPTIONS
    -O3 -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-sign-compare -Wno-implicit-fallthrough -Wno-maybe-uninitialized
    -Wno-type-limits -Wno-missing-field-initializers -Wno-format
    -Wno-pointer-sign -Wno-strict-aliasing -Wno-old-style-declaration
    -Wno-char-subscripts -Wno-attributes)

set_property(SOURCE ${SOLANA_OS_DOOM_SRCS} APPEND PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/apps/doom
    ${SOLANA_OS_DOOM_VENDOR})
set_property(SOURCE ${SOLANA_OS_DOOM_SRCS} APPEND PROPERTY COMPILE_OPTIONS
    -O2 -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-sign-compare -Wno-implicit-fallthrough -Wno-maybe-uninitialized
    -Wno-type-limits -Wno-missing-field-initializers -Wno-format
    -Wno-pointer-sign -Wno-strict-aliasing -Wno-old-style-declaration
    -Wno-char-subscripts -Wno-attributes -Wno-absolute-value
    -Wno-stringop-truncation -Wno-stringop-overflow
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200)
# Walnut-CGB CPU/PPU: -O3 + IRAM on 8-bit dispatch / draw_line (see
# WALNUT_GB_CPU_IRAM). Dual-fetch interpreter stays in flash. gbemu_app.c
# stays -Os (picker, blit, save).
set_property(SOURCE apps/gbemu/walnut_cgb_cpu.c APPEND PROPERTY COMPILE_OPTIONS
    -O3 -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough -Wno-type-limits)

# cJSON (espressif/cjson) parses mesh JSON with a depth cap of 16:
# CONFIG_CJSON_NESTING_LIMIT in sdkconfig.defaults.
