#pragma once

/* Large GBA BSS (IWRAM mirror, IO regs, line buffers) in PSRAM on the badge
 * so boot keeps enough internal SRAM for Wi-Fi + BLE. Same symbols, slower. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define GBA_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define GBA_EXT_BSS
#endif
