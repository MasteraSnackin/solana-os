/* Peanut-GB CPU/interpreter translation unit.
 *
 * gbemu_app.c includes peanut_gb.h with PEANUT_GB_HEADER_ONLY. This file
 * compiles the implementations at -O3 with PEANUT_GB_CPU_IRAM so decode,
 * execute, read, write, gb_run_frame, and __gb_draw_line land in IRAM.
 */

#define ENABLE_SOUND                0
#define ENABLE_LCD                  1
#define PEANUT_GB_IS_LITTLE_ENDIAN  1
/* Sprite sort / 10-per-line off. Faster; busy Yellow lines can glitch. */
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "peanut_gb.h"
