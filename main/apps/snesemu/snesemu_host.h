#pragma once

// C API around the vendored snes9x2002 (PocketSNES) interpreter. The web
// emulator is C-only and does not compile the core; those builds get stubs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNES_WIDTH      256
#define SNES_HEIGHT     224
#define SNES_FB_STRIDE  320   // GFX_PITCH/2; RGB565 pixels per line

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SOLANA_OS_EMULATOR

static inline bool snes_core_boot(const char *rom_path, const char *sav_path)
{
    (void)rom_path;
    (void)sav_path;
    return false;
}

static inline void snes_core_halt(void) {}

static inline int snes_core_frame(uint32_t joy, const uint16_t **fb)
{
    (void)joy;
    (void)fb;
    return 0;
}

static inline void snes_core_save(const char *sav_path, bool force)
{
    (void)sav_path;
    (void)force;
}

static inline bool snes_core_save_dirty(void)
{
    return false;
}

static inline const char *snes_core_title(void)
{
    return "";
}

static inline int snes_core_frameskip(void)
{
    return 3;
}

#else

bool snes_core_boot(const char *rom_path, const char *sav_path);
void snes_core_halt(void);
// Run until a drawn picture. Returns emulated frames consumed (>=1), 0 if none.
// *fb is 256x224 RGB565, stride SNES_FB_STRIDE.
int snes_core_frame(uint32_t joy, const uint16_t **fb);
void snes_core_save(const char *sav_path, bool force);
bool snes_core_save_dirty(void);
const char *snes_core_title(void);
int snes_core_frameskip(void);

#endif

#ifdef __cplusplus
}
#endif
