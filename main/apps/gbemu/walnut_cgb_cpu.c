/* Walnut-CGB CPU/PPU translation unit.
 *
 * gbemu_app.c includes walnut_cgb.h with WALNUT_GB_HEADER_ONLY. This file
 * compiles the implementations at -O3. Use gb_run_frame() (8-bit Peanut
 * dispatch via __gb_step_cpu_x), not gb_run_frame_dualfetch — dual-fetch
 * was slower on ESP32-C5 when the interpreter sat in flash.
 */

#define ENABLE_SOUND                 0
#define ENABLE_LCD                   1
#define WALNUT_GB_IS_LITTLE_ENDIAN   1
#define WALNUT_FULL_GBC_SUPPORT      1
#define WALNUT_GB_12_COLOUR          1
#include "walnut_cgb.h"
