/* Walnut-CGB CPU/PPU translation unit.
 *
 * gbemu_app.c includes walnut_cgb.h with WALNUT_GB_HEADER_ONLY. This file
 * compiles the implementations at -O3 with WALNUT_GB_CPU_IRAM so
 * __gb_step_cpu_x, __gb_read/write, __gb_execute_cb, __gb_draw_line, and
 * gb_run_frame land in IRAM. Dual-fetch (__gb_step_cpu / gb_run_frame_dualfetch)
 * stays in flash — it is unused and ~32 KiB.
 */

#define ENABLE_SOUND                 0
#define ENABLE_LCD                   1
#define WALNUT_GB_IS_LITTLE_ENDIAN   1
#define WALNUT_FULL_GBC_SUPPORT      1
#define WALNUT_GB_12_COLOUR          1
#define WALNUT_GB_CPU_IRAM
#include "walnut_cgb.h"
