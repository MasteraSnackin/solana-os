/* Peanut-GB CPU/interpreter translation unit.
 *
 * gbemu_app.c includes peanut_gb.h with PEANUT_GB_HEADER_ONLY. This file
 * compiles the implementations at -O3 with PEANUT_GB_CPU_IRAM so decode,
 * execute, read, write, and gb_run_frame land in IRAM. LCD stays in flash.
 */

#define ENABLE_SOUND                0
#define ENABLE_LCD                  1
#define PEANUT_GB_IS_LITTLE_ENDIAN  1
#include "peanut_gb.h"
