#pragma once

// C API around the vendored doomgeneric core (Chocolate Doom / ESP-DOOM).
// The web emulator is C-only and does not compile the core; those builds
// get stubs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOOM_WIDTH       320
#define DOOM_HEIGHT      200
#define DOOM_FB_STRIDE   320

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SOLANA_OS_EMULATOR

static inline bool doom_core_boot(const char *wad_path)
{
    (void)wad_path;
    return false;
}

static inline void doom_core_halt(void) {}

static inline int doom_core_tick(uint32_t joy, const uint16_t **fb)
{
    (void)joy;
    (void)fb;
    return 0;
}

static inline bool doom_core_alive(void)
{
    return false;
}

static inline const char *doom_core_title(void)
{
    return "";
}

#else

bool doom_core_boot(const char *wad_path);
void doom_core_halt(void);
// Run one doomgeneric tick. Returns 1 if a new RGB565 frame is ready in *fb
// (320x200, stride DOOM_FB_STRIDE), 0 if no picture this tick, -1 on fatal.
int doom_core_tick(uint32_t joy, const uint16_t **fb);
bool doom_core_alive(void);
const char *doom_core_title(void);

// Called from the vendored engine (I_Error / help exits). Soft-fails the
// current Create/Tick via longjmp instead of aborting the firmware.
void doom_host_fatal(const char *msg);
void doom_host_reset_error(void);

#endif

#ifdef __cplusplus
}
#endif
