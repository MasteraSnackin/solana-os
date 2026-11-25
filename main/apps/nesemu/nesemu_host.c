// Nofrendo glue for Solana OS on ESP32-C5.
// Frame-driven (nes_renderframe), APU muted, 256x224 RGB565, ROM in PSRAM.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "apps/emu_pad.h"
#include "nesemu_host.h"

#include <noftypes.h>
#include <nofconfig.h>
#include <nes.h>
#include <nes_rom.h>
#include <nesinput.h>
#include <nes_ppu.h>
#include <osd.h>
#include <log.h>
#include <gui.h>
#include <event.h>
#include <vid_drv.h>
#include <bitmap.h>
#include <nofrendo.h>

/* Stubs for event.c — we do not run nofrendo's main_loop. */
void main_quit(void) {}
void main_eject(void) {}
void main_insert(const char *filename, system_t type)
{
    (void)filename;
    (void)type;
}

static const char *TAG = "nesemu";

#ifndef NES_FRAMESKIP
#define NES_FRAMESKIP  0   // C5 at 240 MHz usually keeps up; raise if late
#endif

#define NES_ROM_MAX         (2u << 20)   // 2 MB iNES (mapper PRG+CHR)
#define SRAM_BANK_LENGTH    0x0400       // mirrors nes_rom.c

volatile int nofrendo_ticks;

char *osd_getromdata(void);

static bool s_booted;
static bool s_vid_ok;
static nes_t *s_nes;
static uint8_t *s_rom_blob;
static size_t s_rom_bytes;
static uint16_t *s_rgb;
static uint16_t s_pal[256];
static char s_title[64];
static char s_rom_path[256];
static char s_sav_path[256];
static nesinput_t s_pad_inp;
static int s_frameskip = NES_FRAMESKIP;
static int s_skip_i;
static bool s_drawn;
static uint8_t *s_sram_snap;
static size_t s_sram_bytes;
static bool s_save_dirty;
static bitmap_t *s_hw_bmp;
/* vid_init probe only. clear callback is a no-op so size can stay tiny —
 * a full 256x240 stub would steal ~60 KB internal DRAM (BSS) and OOM SD/TLS. */
static uint8_t s_dummy_fb[4];

/* ---- OSD / video ------------------------------------------------------- */

static int vid_init_cb(int width, int height)
{
    (void)width;
    (void)height;
    return 0;
}

static void vid_shutdown_cb(void) {}

static int vid_set_mode_cb(int width, int height)
{
    (void)width;
    (void)height;
    return 0;
}

static void vid_set_palette_cb(rgb_t *pal)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)((pal[i].b >> 3) |
                                ((pal[i].g >> 2) << 5) |
                                ((pal[i].r >> 3) << 11));
        s_pal[i] = c;
    }
}

static void vid_clear_cb(uint8 color)
{
    (void)color;
}

static bitmap_t *vid_lock_write_cb(void)
{
    /* Probe only; primary_buffer from vid_setmode holds real pixels. */
    if (!s_hw_bmp) {
        s_hw_bmp = bmp_createhw(s_dummy_fb, 1, 1, 1);
    }
    return s_hw_bmp;
}

static void vid_free_write_cb(int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties;
    (void)dirty_rects;
}

static void vid_custom_blit_cb(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties;
    (void)dirty_rects;
    if (!bmp || !s_rgb) {
        return;
    }
    /* Visible area is centered in the 240-tall NTSC buffer (8 lines cropped). */
    int y0 = (NES_SCREEN_HEIGHT - NES_VISIBLE_HEIGHT) / 2;
    for (int y = 0; y < NES_HEIGHT; y++) {
        const uint8_t *src = bmp->line[y0 + y];
        uint16_t *dst = s_rgb + (size_t)y * NES_FB_STRIDE;
        for (int x = 0; x < NES_WIDTH; x++) {
            dst[x] = s_pal[src[x]];
        }
    }
    s_drawn = true;
}

static viddriver_t s_vid = {
    .name = "solana-os",
    .init = vid_init_cb,
    .shutdown = vid_shutdown_cb,
    .set_mode = vid_set_mode_cb,
    .set_palette = vid_set_palette_cb,
    .clear = vid_clear_cb,
    .lock_write = vid_lock_write_cb,
    .free_write = vid_free_write_cb,
    .custom_blit = vid_custom_blit_cb,
    .invalidate = false,
};

void osd_getvideoinfo(vidinfo_t *info)
{
    info->default_width = NES_WIDTH;
    info->default_height = NES_HEIGHT;
    info->driver = &s_vid;
}

void osd_getsoundinfo(sndinfo_t *info)
{
    info->sample_rate = 1; /* muted; APU still ticks for IRQ */
    info->bps = 8;
}

void osd_setsound(void (*playfunc)(void *buffer, int size))
{
    (void)playfunc;
}

int osd_init(void)
{
    return 0;
}

void osd_shutdown(void) {}

int osd_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return -1;
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
    (void)frequency;
    (void)func;
    (void)funcsize;
    (void)counter;
    (void)countersize;
    return 0;
}

void osd_fullname(char *filename, const char *shortname)
{
    strncpy(filename, shortname, PATH_MAX);
    filename[PATH_MAX] = 0;
}

char *osd_newextension(char *string, char *ext)
{
    char *dot = strrchr(string, '.');
    char *slash = strrchr(string, '/');
    if (dot && (!slash || dot > slash)) {
        strcpy(dot, ext);
    } else {
        strcat(string, ext);
    }
    return string;
}

void osd_getinput(void) {}

void osd_getmouse(int *x, int *y, int *button)
{
    if (x) {
        *x = 0;
    }
    if (y) {
        *y = 0;
    }
    if (button) {
        *button = 0;
    }
}

int osd_makesnapname(char *filename, int len)
{
    (void)filename;
    (void)len;
    return -1;
}

char *osd_getromdata(void)
{
    return (char *)s_rom_blob;
}

/* ---- ROM load into PSRAM ---------------------------------------------- */

static bool load_rom_blob(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz < 16 || (size_t)sz > NES_ROM_MAX) {
        ESP_LOGE(TAG, "bad ROM size %ld", sz);
        fclose(fp);
        return false;
    }
    rewind(fp);
    uint8_t *blob = heap_caps_malloc((size_t)sz, os_mem_bulk_caps());
    if (!blob) {
        ESP_LOGE(TAG, "PSRAM %ld B for ROM", sz);
        fclose(fp);
        return false;
    }
    if (fread(blob, 1, (size_t)sz, fp) != (size_t)sz) {
        ESP_LOGE(TAG, "short read %s", path);
        heap_caps_free(blob);
        fclose(fp);
        return false;
    }
    fclose(fp);
    if (memcmp(blob, "NES\x1A", 4) != 0) {
        ESP_LOGE(TAG, "not iNES: %s", path);
        heap_caps_free(blob);
        return false;
    }
    s_rom_blob = blob;
    s_rom_bytes = (size_t)sz;
    return true;
}

static void free_rom_blob(void)
{
    heap_caps_free(s_rom_blob);
    s_rom_blob = NULL;
    s_rom_bytes = 0;
}

static uint32_t map_joy(uint32_t joy)
{
    uint32_t d = 0;
    if (joy & EMU_PAD_A) {
        d |= INP_PAD_A;
    }
    if (joy & EMU_PAD_B) {
        d |= INP_PAD_B;
    }
    if (joy & EMU_PAD_SELECT) {
        d |= INP_PAD_SELECT;
    }
    if (joy & EMU_PAD_START) {
        d |= INP_PAD_START;
    }
    if (joy & EMU_PAD_UP) {
        d |= INP_PAD_UP;
    }
    if (joy & EMU_PAD_DOWN) {
        d |= INP_PAD_DOWN;
    }
    if (joy & EMU_PAD_LEFT) {
        d |= INP_PAD_LEFT;
    }
    if (joy & EMU_PAD_RIGHT) {
        d |= INP_PAD_RIGHT;
    }
    return d;
}

static void title_from_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    strlcpy(s_title, base, sizeof(s_title));
    char *dot = strrchr(s_title, '.');
    if (dot) {
        *dot = 0;
    }
}

static void snap_sram(void)
{
    if (!s_nes || !s_nes->rominfo || !s_nes->rominfo->sram) {
        return;
    }
    size_t n = (size_t)SRAM_BANK_LENGTH * (size_t)s_nes->rominfo->sram_banks;
    if (!s_sram_snap || s_sram_bytes != n) {
        heap_caps_free(s_sram_snap);
        s_sram_snap = heap_caps_malloc(n, os_mem_bulk_caps());
        s_sram_bytes = s_sram_snap ? n : 0;
        if (s_sram_snap) {
            memcpy(s_sram_snap, s_nes->rominfo->sram, n);
        }
        s_save_dirty = false;
        return;
    }
    if (memcmp(s_sram_snap, s_nes->rominfo->sram, n) != 0) {
        memcpy(s_sram_snap, s_nes->rominfo->sram, n);
        s_save_dirty = true;
    }
}

bool nes_core_boot(const char *rom_path, const char *sav_path)
{
    if (s_booted) {
        nes_core_halt();
    }
    strlcpy(s_rom_path, rom_path, sizeof(s_rom_path));
    strlcpy(s_sav_path, sav_path ? sav_path : "", sizeof(s_sav_path));
    title_from_path(rom_path);

    if (!load_rom_blob(rom_path)) {
        return false;
    }

    s_rgb = heap_caps_calloc(1, (size_t)NES_WIDTH * NES_HEIGHT * sizeof(uint16_t),
                             os_mem_bulk_caps());
    if (!s_rgb) {
        ESP_LOGE(TAG, "RGB framebuffer");
        free_rom_blob();
        return false;
    }

    log_init();
    event_init();
    (void)config.open(); /* missing nofrendo.cfg is fine */
    if (gui_init()) {
        ESP_LOGE(TAG, "gui_init");
        goto fail;
    }
    if (vid_init(NES_WIDTH, NES_HEIGHT, &s_vid)) {
        ESP_LOGE(TAG, "vid_init");
        goto fail;
    }
    s_vid_ok = true;

    event_set_system(system_nes);
    s_nes = nes_create();
    if (!s_nes) {
        ESP_LOGE(TAG, "nes_create");
        goto fail;
    }

    /* Prefer our .sav path: copy onto rominfo after insert via osd_fullname. */
    if (nes_insertcart(rom_path, s_nes)) {
        ESP_LOGE(TAG, "nes_insertcart %s", rom_path);
        goto fail;
    }
    if (s_sav_path[0] && s_nes->rominfo) {
        /* Force battery path used by rom_savesram / rom_loadsram. */
        strncpy(s_nes->rominfo->filename, s_sav_path, PATH_MAX);
        s_nes->rominfo->filename[PATH_MAX] = 0;
        /* filename currently ends in .sav — rom_* swap extension to .sav again.
           Keep basename.nes so osd_newextension(".sav") lands on our path. */
        strncpy(s_nes->rominfo->filename, rom_path, PATH_MAX);
        s_nes->rominfo->filename[PATH_MAX] = 0;
    }

    /* PPU writes scanlines 0..239; custom_blit crops 8 lines top/bottom to
     * NES_HEIGHT (224). A 224-tall primary_buffer OOB-reads line[224..] and
     * panics (Load access fault) on the first vid_flush. */
    if (vid_setmode(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT)) {
        ESP_LOGE(TAG, "vid_setmode");
        goto fail;
    }

    memset(&s_pad_inp, 0, sizeof(s_pad_inp));
    s_pad_inp.type = INP_JOYPAD0;
    input_register(&s_pad_inp);

    s_nes->scanline_cycles = 0;
    s_nes->fiq_cycles = (int)(236250000 / 11 / 12 / 60);
    s_nes->autoframeskip = false;
    s_skip_i = 0;
    s_drawn = false;
    s_save_dirty = false;
    snap_sram();
    s_booted = true;
    ESP_LOGI(TAG, "booted %s (%u B iNES, mapper %d)", s_title,
             (unsigned)s_rom_bytes,
             s_nes->rominfo ? s_nes->rominfo->mapper_number : -1);
    return true;

fail:
    nes_core_halt();
    return false;
}

void nes_core_halt(void)
{
    if (s_nes) {
        if (s_sav_path[0] && s_nes->rominfo) {
            strncpy(s_nes->rominfo->filename, s_rom_path, PATH_MAX);
            s_nes->rominfo->filename[PATH_MAX] = 0;
        }
        nes_destroy(&s_nes);
        s_nes = NULL;
    }
    if (s_vid_ok) {
        vid_shutdown();
        s_vid_ok = false;
    }
    gui_shutdown();
    if (s_hw_bmp) {
        bmp_destroy(&s_hw_bmp);
        s_hw_bmp = NULL;
    }
    heap_caps_free(s_rgb);
    s_rgb = NULL;
    heap_caps_free(s_sram_snap);
    s_sram_snap = NULL;
    s_sram_bytes = 0;
    free_rom_blob();
    s_booted = false;
    s_drawn = false;
    s_save_dirty = false;
}

int nes_core_frame(uint32_t joy, const uint16_t **fb)
{
    if (!s_booted || !s_nes) {
        return 0;
    }
    s_pad_inp.data = (int)map_joy(joy);
    bool draw = (s_skip_i == 0);
    s_drawn = false;
    nes_renderframe(draw);
    if (draw) {
        vid_flush();
    }
    s_skip_i++;
    if (s_skip_i > s_frameskip) {
        s_skip_i = 0;
    }
    snap_sram();
    if (fb) {
        *fb = (draw && s_drawn) ? s_rgb : NULL;
    }
    return 1;
}

void nes_core_save(const char *sav_path, bool force)
{
    if (!s_nes || !s_nes->rominfo || !s_nes->rominfo->sram) {
        return;
    }
    if (!force && !s_save_dirty) {
        return;
    }
    if (!(s_nes->rominfo->flags & ROM_FLAG_BATTERY)) {
        return;
    }
    const char *path = (sav_path && sav_path[0]) ? sav_path : s_sav_path;
    if (!path[0] || !s_nes->rominfo->sram) {
        return;
    }
    size_t n = (size_t)SRAM_BANK_LENGTH * (size_t)s_nes->rominfo->sram_banks;
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "save open %s", path);
        return;
    }
    size_t w = fwrite(s_nes->rominfo->sram, 1, n, fp);
    fclose(fp);
    if (w == n) {
        s_save_dirty = false;
        ESP_LOGI(TAG, "saved %u B -> %s", (unsigned)n, path);
    }
}

bool nes_core_save_dirty(void)
{
    return s_save_dirty;
}

const char *nes_core_title(void)
{
    return s_title;
}

int nes_core_frameskip(void)
{
    return s_frameskip;
}
