// 44VBA / vba-next glue for Solana OS. Allocates GBA RAM in PSRAM, loads a
// .gba from the microSD card, and presents 240x160 RGB565 frames.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

extern "C" {
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core/os.h"
#include "core/os_mem.h"
}
#include "gbaemu_host.h"

#undef B0
#undef B1
#undef B2
#undef B3
#include "gba.h"
#include "globals.h"
#include "memory.h"
#include "system.h"
#include "gba_rom_page.h"

static const char *TAG = "gbaemu";

#define ROM_MAX            (16u << 20)
#define PIX_BYTES          (4 * PIX_BUFFER_SCREEN_WIDTH * 160)
#define GBA_RAM_RESERVE    0x140000u
// Up to 16×256 KB (4 MB). Eight windows let GTA's 3D attract run faster but
// Fire Red's intro DMA thrashed them (~0.3 emu fps, hours to reach the
// title). Sixteen windows hold that intro; alloc falls back to 8 if PSRAM is
// tight after Game Boy. Tags 0–3 (first 1 MB: vectors + ARM) stay pinned.
// Pinning more of Doom (20 windows / 2 MB pin) did not raise in-game fps.
#define GBA_ROM_PAGES      16
#define GBA_ROM_PAGE_SIZE  (1u << GBA_ROM_PAGE_SHIFT)
#define GBA_ROM_PIN_TAGS   4

uint8_t *gba_rom_direct;
uint8_t *gba_rom_hot;
uint8_t *gba_rom_hot2;
uint32_t gba_rom_hot_tag = ~0u;
uint32_t gba_rom_hot2_tag = ~0u;

static volatile int s_drawn;
static char s_title[13];
static char s_sav[200];
static uint8_t *s_save_snap;
static bool s_booted;
static bool s_rom_paged;
static FILE *s_rom_file;
static size_t s_rom_size;
static uint8_t *s_rom_pool;
static int s_rom_pages;
static int s_rom_pin_tags = GBA_ROM_PIN_TAGS;
static int s_frameskip = 2;
static uint8_t *s_rom_slot[GBA_ROM_PAGES];
static uint32_t s_rom_tag[GBA_ROM_PAGES];
static uint32_t s_rom_stamp[GBA_ROM_PAGES];
static uint32_t s_rom_tick;
static uint32_t s_rom_misses;
static bool s_flash128;

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: SRAM %u free, PSRAM %u free, largest PSRAM block %u",
             when,
             (unsigned)heap_caps_get_free_size(OS_MEM_SRAM),
             (unsigned)heap_caps_get_free_size(OS_MEM_PSRAM),
             (unsigned)heap_caps_get_largest_free_block(OS_MEM_PSRAM));
}

static uint8_t *ps_alloc(size_t n, const char *what)
{
    uint8_t *p = (uint8_t *)heap_caps_malloc(n, os_mem_bulk_caps());
    if (!p) {
        ESP_LOGE(TAG, "alloc %s %u B failed (SRAM %u free, PSRAM %u free, largest %u)",
                 what, (unsigned)n,
                 (unsigned)heap_caps_get_free_size(OS_MEM_SRAM),
                 (unsigned)heap_caps_get_free_size(OS_MEM_PSRAM),
                 (unsigned)heap_caps_get_largest_free_block(OS_MEM_PSRAM));
    }
    return p;
}

static size_t pow2_ge(size_t n)
{
    size_t p = 1;
    while (p < n) {
        p <<= 1;
        if (!p) {
            return 0;
        }
    }
    return p;
}

void systemMessage(const char *fmt, ...)
{
    if (!fmt) {
        return;
    }
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ESP_LOGW("GBA", "%s", buf);
}

#if USE_FRAME_SKIP
extern bool fs_draw;
#endif

void systemDrawScreen(void)
{
#if USE_FRAME_SKIP
    // fs_draw still describes this frame's scanlines (updated after this call).
    if (!fs_draw) {
        return;
    }
#endif
    s_drawn = 1;
}

void systemOnWriteDataToSoundBuffer(int16_t *finalWave, int length)
{
    (void)finalWave;
    (void)length;
}

bool systemReadJoypads(void)
{
    return true;
}

uint32_t systemGetClock(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void rom_page_reset(void)
{
    if (s_rom_file) {
        fclose(s_rom_file);
        s_rom_file = NULL;
    }
    if (s_rom_paged) {
        heap_caps_free(s_rom_pool);
        s_rom_pool = NULL;
        for (int i = 0; i < GBA_ROM_PAGES; i++) {
            s_rom_slot[i] = NULL;
            s_rom_tag[i] = ~0u;
            s_rom_stamp[i] = 0;
        }
        s_rom_pages = 0;
        rom = NULL;
    } else {
        heap_caps_free(rom);
        rom = NULL;
    }
    gba_rom_direct = NULL;
    gba_rom_hot = NULL;
    gba_rom_hot2 = NULL;
    gba_rom_hot_tag = ~0u;
    gba_rom_hot2_tag = ~0u;
    s_rom_paged = false;
    s_rom_size = 0;
    s_rom_tick = 0;
    s_rom_misses = 0;
    s_flash128 = false;
}

static void free_core(void)
{
    heap_caps_free(workRAM);
    heap_caps_free(vram);
    heap_caps_free(bios);
    heap_caps_free(pix);
    heap_caps_free(libretro_save_buf);
    heap_caps_free(s_save_snap);
    rom_page_reset();
    workRAM = vram = bios = libretro_save_buf = NULL;
    pix = NULL;
    s_save_snap = NULL;
    s_booted = false;
    s_title[0] = 0;
}

static bool read_window(FILE *f, uint32_t base, uint8_t *dst, size_t n);

static void rom_fill_slot(int slot, uint32_t base)
{
    uint8_t *dst = s_rom_slot[slot];
    size_t n = GBA_ROM_PAGE_SIZE;
    if (base >= s_rom_size) {
        memset(dst, 0, GBA_ROM_PAGE_SIZE);
        return;
    }
    if (base + n > s_rom_size) {
        n = s_rom_size - base;
    }
    // sd_do_transaction already holds the LCD around each SPI command.
    // Nested bsp_lcd_hold here deadlocks (binary semaphore, not recursive).
    if (!read_window(s_rom_file, base, dst, n)) {
        ESP_LOGW(TAG, "ROM page 0x%lx read failed", (unsigned long)base);
        memset(dst, 0xFF, GBA_ROM_PAGE_SIZE);
        n = 0;
    }
    if (n < GBA_ROM_PAGE_SIZE) {
        memset(dst + n, 0, GBA_ROM_PAGE_SIZE - n);
    }
}

static int IRAM_ATTR rom_find_tag(uint32_t tag)
{
    for (int i = 0; i < s_rom_pages; i++) {
        if (s_rom_tag[i] == tag) {
            return i;
        }
    }
    return -1;
}

static int IRAM_ATTR rom_lru_except(int keep)
{
    int victim = -1;
    uint32_t oldest = ~0u;
    for (int i = 0; i < s_rom_pages; i++) {
        if (i == keep || (int)s_rom_tag[i] < s_rom_pin_tags) {
            continue;
        }
        if (victim < 0 || s_rom_stamp[i] <= oldest) {
            oldest = s_rom_stamp[i];
            victim = i;
        }
    }
    if (victim >= 0) {
        return victim;
    }
    victim = keep == 0 ? 1 : 0;
    oldest = s_rom_stamp[victim];
    for (int i = 0; i < s_rom_pages; i++) {
        if (i != keep && s_rom_stamp[i] <= oldest) {
            oldest = s_rom_stamp[i];
            victim = i;
        }
    }
    return victim;
}

static void IRAM_ATTR rom_promote(int slot, uint32_t tag)
{
    if (tag != gba_rom_hot_tag) {
        gba_rom_hot2 = gba_rom_hot;
        gba_rom_hot2_tag = gba_rom_hot_tag;
        gba_rom_hot = s_rom_slot[slot];
        gba_rom_hot_tag = tag;
    }
    s_rom_stamp[slot] = ++s_rom_tick;
}

IRAM_ATTR uint8_t *gba_rom_map(uint32_t off)
{
    uint32_t tag = off >> GBA_ROM_PAGE_SHIFT;
    int hit = rom_find_tag(tag);
    int slot = hit >= 0 ? hit : rom_lru_except(-1);
    if (hit < 0) {
        s_rom_misses++;
        rom_fill_slot(slot, tag << GBA_ROM_PAGE_SHIFT);
        s_rom_tag[slot] = tag;
        if (s_rom_misses <= 4 || (s_rom_misses & 0x3F) == 0) {
            ESP_LOGI(TAG, "ROM page miss #%u at 0x%lx", (unsigned)s_rom_misses,
                     (unsigned long)off);
        }
        uint32_t next = tag + 1;
        if ((next << GBA_ROM_PAGE_SHIFT) < s_rom_size && rom_find_tag(next) < 0) {
            int pref = rom_lru_except(slot);
            rom_fill_slot(pref, next << GBA_ROM_PAGE_SHIFT);
            s_rom_tag[pref] = next;
            s_rom_stamp[pref] = ++s_rom_tick;
        }
    }
    rom_promote(slot, tag);
    return s_rom_slot[slot] + (off & GBA_ROM_PAGE_MASK);
}

static bool load_path(const char *path, uint8_t **out, size_t *size, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && (size_t)n <= max;
    uint8_t *buf = ok ? ps_alloc((size_t)n, path) : NULL;
    ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        heap_caps_free(buf);
        return false;
    }
    *out = buf;
    *size = (size_t)n;
    return true;
}

// SD and the ILI9341 share SPI2. One-shot 256 KB freads fail under a live
// blit; 4 MB carts already retried in 256 KB chunks. Paged 8/16 MB carts
// (Doom, GTA) need the same retry or a miss fills 0xFF and the CPU hangs
// on a black screen.
static bool read_window(FILE *f, uint32_t base, uint8_t *dst, size_t n)
{
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > (256u << 10)) {
            chunk = 256u << 10;
        }
        int tries = 0;
        for (;;) {
            if (fseek(f, (long)base + (long)off, SEEK_SET) == 0 &&
                    fread(dst + off, 1, chunk, f) == chunk) {
                break;
            }
            if (++tries >= 5) {
                return false;
            }
            clearerr(f);
            vTaskDelay(1);
        }
        off += chunk;
        vTaskDelay(1);
    }
    return true;
}

static bool load_rom_direct(FILE *f, size_t n)
{
    rom = ps_alloc(n, "ROM");
    if (!rom) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    if (!read_window(f, 0, rom, n)) {
        ESP_LOGE(TAG, "ROM read failed (%u B)", (unsigned)n);
        heap_caps_free(rom);
        rom = NULL;
        return false;
    }
    fclose(f);
    gba_rom_direct = rom;
    gba_rom_hot = rom;
    gba_rom_hot2 = rom;
    gba_rom_hot_tag = 0;
    gba_rom_hot2_tag = ~0u;
    s_rom_paged = false;
    s_rom_file = NULL;
    s_rom_size = n;
    ESP_LOGI(TAG, "ROM in PSRAM (%u KB) in %d ms",
             (unsigned)(n >> 10), (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static bool load_rom_paged(FILE *f, size_t n)
{
    int pages = GBA_ROM_PAGES;
    int pin = GBA_ROM_PIN_TAGS;
    s_rom_pool = ps_alloc((size_t)pages * GBA_ROM_PAGE_SIZE, "ROM cache");
    if (!s_rom_pool) {
        pages = 8;
        pin = 2;
        s_rom_pool = ps_alloc((size_t)pages * GBA_ROM_PAGE_SIZE, "ROM cache");
    }
    if (!s_rom_pool) {
        return false;
    }
    s_rom_pages = pages;
    s_rom_pin_tags = pin;
    for (int i = 0; i < GBA_ROM_PAGES; i++) {
        s_rom_slot[i] = NULL;
        s_rom_tag[i] = ~0u;
        s_rom_stamp[i] = 0;
    }
    for (int i = 0; i < pages; i++) {
        s_rom_slot[i] = s_rom_pool + (size_t)i * GBA_ROM_PAGE_SIZE;
        s_rom_tag[i] = ~0u;
        s_rom_stamp[i] = 0;
    }
    s_rom_file = f;
    s_rom_size = n;
    s_rom_paged = true;
    s_rom_tick = 0;
    s_rom_misses = 0;
    gba_rom_direct = NULL;
    rom = s_rom_slot[0];

    int nfill = pages;
    int file_pages = (int)((n + GBA_ROM_PAGE_SIZE - 1) / GBA_ROM_PAGE_SIZE);
    if (nfill > file_pages) {
        nfill = file_pages;
    }
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < nfill; i++) {
        rom_fill_slot(i, (uint32_t)i * GBA_ROM_PAGE_SIZE);
        s_rom_tag[i] = (uint32_t)i;
        s_rom_stamp[i] = ++s_rom_tick;
        vTaskDelay(1);
    }
    gba_rom_hot = s_rom_slot[0];
    gba_rom_hot2 = nfill > 1 ? s_rom_slot[1] : s_rom_slot[0];
    gba_rom_hot_tag = 0;
    gba_rom_hot2_tag = nfill > 1 ? 1u : ~0u;
    ESP_LOGI(TAG, "ROM paged from SD (%u KB file, %dx%u KB windows, pin %d tags / %u KB, %d prefilled in %d ms)",
             (unsigned)(n >> 10), pages, (unsigned)(GBA_ROM_PAGE_SIZE >> 10),
             pin, (unsigned)((uint32_t)pin * (GBA_ROM_PAGE_SIZE >> 10)), nfill,
             (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

// Fire Red / Ultra Violet hang on a white screen if the chip ID is 64K Sanyo
// (44VBA default). FLASH1M_Vxxx wants 128K Macronix.
static bool buf_has_id(const uint8_t *p, size_t n, const char *id)
{
    size_t idn = strlen(id);
    if (!p || n < idn) {
        return false;
    }
    for (size_t i = 0; i + idn <= n; i++) {
        if (p[i] == (uint8_t)id[0] && memcmp(p + i, id, idn) == 0) {
            return true;
        }
    }
    return false;
}

static bool file_has_id(FILE *f, size_t n, const char *id)
{
    size_t idn = strlen(id);
    const size_t chunk = 64u << 10;
    uint8_t *buf = ps_alloc(chunk + idn, "save-id scan");
    if (!buf || !f) {
        heap_caps_free(buf);
        return false;
    }
    long saved = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t off = 0, carry = 0;
    bool found = false;
    while (off < n) {
        size_t want = chunk;
        if (want > n - off) {
            want = n - off;
        }
        if (fread(buf + carry, 1, want, f) != want) {
            clearerr(f);
            fseek(f, (long)off, SEEK_SET);
            if (fread(buf + carry, 1, want, f) != want) {
                break;
            }
        }
        size_t have = want + carry;
        if (buf_has_id(buf, have, id)) {
            found = true;
            break;
        }
        carry = idn > 1 ? idn - 1 : 0;
        if (carry > have) {
            carry = have;
        }
        memmove(buf, buf + have - carry, carry);
        off += want;
    }
    fseek(f, saved < 0 ? 0 : saved, SEEK_SET);
    heap_caps_free(buf);
    return found;
}

static void detect_flash128(void)
{
    const char *id = "FLASH1M_V";
    if (s_rom_paged) {
        for (int i = 0; i < s_rom_pages && !s_flash128; i++) {
            if (s_rom_slot[i] && buf_has_id(s_rom_slot[i], GBA_ROM_PAGE_SIZE, id)) {
                s_flash128 = true;
            }
        }
        if (!s_flash128 && s_rom_file) {
            s_flash128 = file_has_id(s_rom_file, s_rom_size, id);
        }
    } else if (rom && s_rom_size) {
        s_flash128 = buf_has_id(rom, s_rom_size, id);
    }
    if (s_flash128) {
        ESP_LOGI(TAG, "ROM id FLASH1M -> 128K flash + RTC");
    }
}

// Whole cart in PSRAM when it still leaves room for GBA RAM. Otherwise a
// small SD window cache (not a 32 MB pad).
static bool load_rom(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (size_t)n > ROM_MAX) {
        ESP_LOGE(TAG, "%s is %ld B (max %u)", path, n, (unsigned)ROM_MAX);
        fclose(f);
        return false;
    }

    log_heap("before ROM alloc");
    size_t largest = heap_caps_get_largest_free_block(OS_MEM_PSRAM);
    bool direct = (size_t)n + GBA_RAM_RESERVE <= largest;
    bool ok = direct ? load_rom_direct(f, (size_t)n) : load_rom_paged(f, (size_t)n);
    if (!ok) {
        if (s_rom_file != f) {
            fclose(f);
        }
        rom_page_reset();
        return false;
    }
    *size = (size_t)n;
    detect_flash128();
    log_heap("after ROM load");
    return true;
}

extern "C" bool gba_core_boot(const char *rom_path, const char *sav_path)
{
    gba_core_halt();
    log_heap("GBA boot");

    size_t file_size = 0;
    if (!load_rom(rom_path, &file_size)) {
        os_toast("Can't read that ROM (max 16 MB)");
        return false;
    }

    size_t wrap = pow2_ge(file_size);
    if (!wrap || wrap > ROM_MAX) {
        free_core();
        os_toast("ROM is too big (max 16 MB)");
        return false;
    }

    workRAM = ps_alloc(0x40000, "workRAM");
    vram = ps_alloc(0x20000, "vram");
    bios = ps_alloc(0x4000, "bios");
    pix = (uint16_t *)ps_alloc(PIX_BYTES, "pix");
    libretro_save_buf = ps_alloc(LIBRETRO_SAVE_BUF_LEN, "save");
    s_save_snap = ps_alloc(LIBRETRO_SAVE_BUF_LEN, "save snap");
    if (!workRAM || !vram || !bios || !pix || !libretro_save_buf || !s_save_snap) {
        free_core();
        os_toast("Out of memory for GBA");
        return false;
    }
    memset(pix, 0, PIX_BYTES);

    if (file_size >= 0xAC + 12) {
        memcpy(s_title, gba_rom_ptr(0xA0), 12);
        s_title[12] = 0;
        for (int i = 11; i >= 0 && s_title[i] == ' '; i--) {
            s_title[i] = 0;
        }
    }

    strlcpy(s_sav, sav_path ? sav_path : "", sizeof(s_sav));

    if (!CPUSetupBuffers()) {
        free_core();
        os_toast("GBA buffers failed");
        return false;
    }
    CPUSetRomMask((uint32_t)wrap - 1);
    CPUInit(NULL, false);
    CPUReset();
    CPUSetRomMask((uint32_t)wrap - 1);
    if (s_flash128) {
        flashSetSize(FLASH_128K_SZ);
        rtcEnable(true);
    }
    // Skip 2 (draw 1 of 3) on tiled modes. gba_core_frame raises that to
    // skip 5 in bitmap modes 3/4/5 so Doom's Hangar gets more game time.
    s_frameskip = 2;
    SetFrameskip(s_frameskip);

    // flashInit() filled save RAM with 0xFF; overlay a .sav after that.
    if (s_sav[0]) {
        uint8_t *sav = NULL;
        size_t got = 0;
        if (load_path(s_sav, &sav, &got, LIBRETRO_SAVE_BUF_LEN) && got) {
            memcpy(libretro_save_buf, sav, got);
        }
        heap_caps_free(sav);
    }
    memcpy(s_save_snap, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN);

    s_drawn = 0;
    s_booted = true;
    log_heap("GBA ready");
    ESP_LOGI(TAG, "%s: %u KB ROM loaded ok, %s, mask 0x%lx",
             s_title[0] ? s_title : "GBA",
             (unsigned)(file_size >> 10),
             s_rom_paged ? "paged from SD" : "in PSRAM",
             (unsigned long)(wrap - 1));
    return true;
}

extern "C" void gba_core_halt(void)
{
    if (s_booted) {
        gba_core_save(s_sav, false);
    }
    free_core();
}

extern "C" int gba_core_frame(uint32_t buttons, const uint16_t **fb)
{
    if (!s_booted) {
        return 0;
    }
    s_drawn = 0;
    int n = 0;
    // Bitmap modes 3/4/5 are Doom's 3D / heavy frames. Extra skip there
    // without a global skip on tiled games (Advance Wars stays skip 2).
#ifndef GBA_BITMAP_SKIP
#define GBA_BITMAP_SKIP  5
#endif
    {
        int mode = (int)(ioMem[0] & 7);
        int want = (mode >= 3) ? GBA_BITMAP_SKIP : 2;
        if (want != s_frameskip) {
            s_frameskip = want;
            SetFrameskip(s_frameskip);
        }
    }
    // Skip 2 runs ~3 PPU frames per call (~200–300 ms when paging). Re-read
    // the console inject each PPU frame so `key start` is not eaten by a
    // long CPULoop that began before the inject. Cap covers skip 5 (1 of 6).
    while (!s_drawn && n < 12) {
        joy = buttons | os_pad_injected();
        UpdateJoypad();
        CPULoop();
        n++;
    }
    if (fb) {
        *fb = pix;
    }
    return s_drawn ? n : 0;
}

extern "C" bool gba_core_save_dirty(void)
{
    return s_booted && s_save_snap &&
           memcmp(s_save_snap, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN) != 0;
}

extern "C" void gba_core_save(const char *sav_path, bool force)
{
    const char *path = sav_path && sav_path[0] ? sav_path : s_sav;
    if (!s_booted || !path[0] || !libretro_save_buf) {
        return;
    }
    if (!force && !gba_core_save_dirty()) {
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f && fwrite(libretro_save_buf, 1, LIBRETRO_SAVE_BUF_LEN, f) == LIBRETRO_SAVE_BUF_LEN) {
        memcpy(s_save_snap, libretro_save_buf, LIBRETRO_SAVE_BUF_LEN);
        ESP_LOGI(TAG, "saved %u B to %s", (unsigned)LIBRETRO_SAVE_BUF_LEN, path);
        if (force) {
            os_toast("Saved");
        }
    } else {
        ESP_LOGW(TAG, "save to %s failed", path);
        if (force) {
            os_toast("Save failed");
        }
    }
    if (f) {
        fclose(f);
    }
}

extern "C" const char *gba_core_title(void)
{
    return s_title;
}

extern "C" uint32_t gba_core_rom_misses(void)
{
    return s_rom_misses;
}

extern "C" int gba_core_frameskip(void)
{
    return s_frameskip;
}

extern "C" int gba_core_video_mode(void)
{
    return s_booted ? (int)(ioMem[0] & 7) : 0;
}
