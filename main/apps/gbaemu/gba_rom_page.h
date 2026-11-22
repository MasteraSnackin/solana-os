#pragma once

// 44VBA ROM reads. Direct: whole cart in PSRAM. Paged: SD window cache.
// gba_rom_map() is the miss path (host.cpp).

#include <stdint.h>
#include "port.h"

#define GBA_ROM_REGION(addr) ((((uint32_t)(addr)) >> 24) - 8u <= 4u)
#define GBA_ROM_PAGE_SHIFT   18u
#define GBA_ROM_PAGE_MASK    ((1u << GBA_ROM_PAGE_SHIFT) - 1u)

extern uint8_t *gba_rom_direct;
extern uint8_t *gba_rom_hot;
extern uint32_t gba_rom_hot_tag;
extern uint32_t romMask;

uint8_t *gba_rom_map(uint32_t off);

static inline uint8_t *gba_rom_ptr(uint32_t off)
{
    off &= romMask;
    if (gba_rom_direct) {
        return gba_rom_direct + off;
    }
    if ((off >> GBA_ROM_PAGE_SHIFT) == gba_rom_hot_tag) {
        return gba_rom_hot + (off & GBA_ROM_PAGE_MASK);
    }
    return gba_rom_map(off);
}

static inline uint8_t gba_rom_r8(uint32_t off)
{
    return *gba_rom_ptr(off);
}

static inline uint16_t gba_rom_r16(uint32_t off)
{
    return READ16LE(gba_rom_ptr(off & ~1u));
}

static inline uint32_t gba_rom_r32(uint32_t off)
{
    return READ32LE(gba_rom_ptr(off & ~3u));
}
