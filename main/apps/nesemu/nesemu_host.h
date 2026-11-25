#pragma once

// C API around the vendored Nofrendo NES core. The web emulator is C-only
// and does not compile the core; those builds get stubs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NES_WIDTH       256
#define NES_HEIGHT      224
#define NES_FB_STRIDE   256

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SOLANA_OS_EMULATOR

static inline bool nes_core_boot(const char *rom_path, const char *sav_path)
{
    (void)rom_path;
    (void)sav_path;
    return false;
}

static inline void nes_core_halt(void) {}

static inline int nes_core_frame(uint32_t joy, const uint16_t **fb)
{
    (void)joy;
    (void)fb;
    return 0;
}

static inline void nes_core_save(const char *sav_path, bool force)
{
    (void)sav_path;
    (void)force;
}

static inline bool nes_core_save_dirty(void)
{
    return false;
}

static inline const char *nes_core_title(void)
{
    return "";
}

static inline int nes_core_frameskip(void)
{
    return 0;
}

#else

bool nes_core_boot(const char *rom_path, const char *sav_path);
void nes_core_halt(void);
// Run one emulated frame (plus frameskip). Returns emulated frames (>=1), 0 if none.
// *fb is 256x224 RGB565, stride NES_FB_STRIDE, only when a picture was drawn.
int nes_core_frame(uint32_t joy, const uint16_t **fb);
void nes_core_save(const char *sav_path, bool force);
bool nes_core_save_dirty(void);
const char *nes_core_title(void);
int nes_core_frameskip(void);

#endif

#ifdef __cplusplus
}
#endif
