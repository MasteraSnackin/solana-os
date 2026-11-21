#pragma once

// C API around the vendored 44VBA (vba-next) core. The web emulator is C-only
// and does not compile the C++ core; those builds get the inline stubs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GBA_WIDTH   240
#define GBA_HEIGHT  160

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SOLANA_OS_EMULATOR

static inline bool gba_core_boot(const char *rom_path, const char *sav_path)
{
    (void)rom_path;
    (void)sav_path;
    return false;
}

static inline void gba_core_halt(void) {}

static inline bool gba_core_frame(uint32_t joy, const uint16_t **fb)
{
    (void)joy;
    (void)fb;
    return false;
}

static inline void gba_core_save(const char *sav_path, bool force)
{
    (void)sav_path;
    (void)force;
}

static inline bool gba_core_save_dirty(void)
{
    return false;
}

static inline const char *gba_core_title(void)
{
    return "";
}

#else

bool gba_core_boot(const char *rom_path, const char *sav_path);
void gba_core_halt(void);
// Run until the PPU emits a frame. *fb is 240x160 RGB565, stride 240.
bool gba_core_frame(uint32_t joy, const uint16_t **fb);
void gba_core_save(const char *sav_path, bool force);
bool gba_core_save_dirty(void);
const char *gba_core_title(void);

#endif

#ifdef __cplusplus
}
#endif
