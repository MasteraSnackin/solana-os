#pragma once

// C API around the vendored 44VBA (vba-next) core. The web emulator is C-only
// and does not compile the C++ core; those builds get the inline stubs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GBA_WIDTH      240
#define GBA_HEIGHT     160
#define GBA_FB_STRIDE  256   // 44VBA pix buffer width; LVGL stride is this * 2

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

static inline int gba_core_frame(uint32_t joy, const uint16_t **fb)
{
    (void)joy;
    (void)fb;
    return 0;
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

static inline uint32_t gba_core_rom_misses(void)
{
    return 0;
}

static inline int gba_core_frameskip(void)
{
    return 2;
}

static inline int gba_core_video_mode(void)
{
    return 0;
}

#else

bool gba_core_boot(const char *rom_path, const char *sav_path);
void gba_core_halt(void);
// Run until a drawn picture. Returns emulated frames consumed (>=1), 0 if none.
// *fb is 240x160 RGB565, stride GBA_FB_STRIDE.
int gba_core_frame(uint32_t joy, const uint16_t **fb);
void gba_core_save(const char *sav_path, bool force);
bool gba_core_save_dirty(void);
const char *gba_core_title(void);
uint32_t gba_core_rom_misses(void);
int gba_core_frameskip(void);
int gba_core_video_mode(void);

#endif

#ifdef __cplusplus
}
#endif
