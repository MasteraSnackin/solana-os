#pragma once

/* Large BSS (maps, echo, DMA scratch) lives in PSRAM on the badge. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SNES_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define SNES_EXT_BSS
#endif
