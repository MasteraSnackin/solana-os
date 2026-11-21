// 44VBA / vba-next glue for Solana OS. Allocates GBA RAM in PSRAM, loads a
// .gba from the microSD card, and presents 240x160 RGB565 frames.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

extern "C" {
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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

static const char *TAG = "gbaemu";

#define ROM_MAX         (8u << 20)
#define PIX_BYTES       (4 * PIX_BUFFER_SCREEN_WIDTH * 160)

static uint16_t *s_fb;
static volatile int s_drawn;
static char s_title[13];
static char s_sav[200];
static uint8_t *s_save_snap;
static bool s_booted;

static uint8_t *ps_alloc(size_t n)
{
    return (uint8_t *)heap_caps_calloc(1, n, os_mem_bulk_caps());
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

void systemDrawScreen(void)
{
    const uint16_t *src = pix;
    uint16_t *dst = s_fb;
    for (int y = 0; y < GBA_HEIGHT; y++) {
        memcpy(dst, src, GBA_WIDTH * sizeof(uint16_t));
        dst += GBA_WIDTH;
        src += PIX_BUFFER_SCREEN_WIDTH;
    }
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

static void free_core(void)
{
    heap_caps_free(workRAM);
    heap_caps_free(vram);
    heap_caps_free(bios);
    heap_caps_free(pix);
    heap_caps_free(libretro_save_buf);
    heap_caps_free(rom);
    heap_caps_free(s_save_snap);
    heap_caps_free(s_fb);
    workRAM = vram = bios = libretro_save_buf = rom = NULL;
    pix = NULL;
    s_save_snap = NULL;
    s_fb = NULL;
    s_booted = false;
    s_title[0] = 0;
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
    uint8_t *buf = ok ? ps_alloc((size_t)n) : NULL;
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

extern "C" bool gba_core_boot(const char *rom_path, const char *sav_path)
{
    gba_core_halt();

    uint8_t *file = NULL;
    size_t file_size = 0;
    if (!load_path(rom_path, &file, &file_size, ROM_MAX)) {
        os_toast("Can't read that ROM (max 8 MB)");
        return false;
    }

    size_t alloc = pow2_ge(file_size);
    if (!alloc || alloc > ROM_MAX) {
        heap_caps_free(file);
        os_toast("ROM is too big (need a power-of-two fit, max 8 MB)");
        return false;
    }

    rom = ps_alloc(alloc);
    workRAM = ps_alloc(0x40000);
    vram = ps_alloc(0x20000);
    bios = ps_alloc(0x4000);
    pix = (uint16_t *)ps_alloc(PIX_BYTES);
    libretro_save_buf = ps_alloc(LIBRETRO_SAVE_BUF_LEN);
    s_save_snap = ps_alloc(LIBRETRO_SAVE_BUF_LEN);
    s_fb = (uint16_t *)ps_alloc(GBA_WIDTH * GBA_HEIGHT * sizeof(uint16_t));
    if (!rom || !workRAM || !vram || !bios || !pix || !libretro_save_buf || !s_save_snap || !s_fb) {
        heap_caps_free(file);
        free_core();
        os_toast("Out of memory for GBA");
        return false;
    }
    memcpy(rom, file, file_size);
    heap_caps_free(file);

    if (file_size >= 0xAC + 12) {
        memcpy(s_title, rom + 0xA0, 12);
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
    CPUInit(NULL, false);
    CPUReset();
    CPUSetRomMask((uint32_t)alloc - 1);
    SetFrameskip(0x1);

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
    ESP_LOGI(TAG, "%s: %u KB ROM, mask 0x%lx", s_title[0] ? s_title : "GBA",
             (unsigned)(file_size >> 10), (unsigned long)(alloc - 1));
    return true;
}

extern "C" void gba_core_halt(void)
{
    if (s_booted) {
        gba_core_save(s_sav, false);
    }
    free_core();
}

extern "C" bool gba_core_frame(uint32_t buttons, const uint16_t **fb)
{
    if (!s_booted) {
        return false;
    }
    joy = buttons;
    UpdateJoypad();
    s_drawn = 0;
    int guard = 0;
    while (!s_drawn && guard++ < 8) {
        CPULoop();
    }
    if (fb) {
        *fb = s_fb;
    }
    return s_drawn != 0;
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
