// Game Boy: plays a .gb / .gbc ROM from the microSD card (/sdcard/roms).
//
// The CPU/PPU core is Peanut-GB (peanut_gb.h, MIT, vendored next to this file).
// peanut_gb_cpu.c builds the interpreter and LCD line at -O3 in IRAM.
// ROM bank 0 is copied into a 16 KB SRAM window while a ROM runs (32 KB does not fit).
// No ROM ships with the firmware: copy a legally owned dump onto the card.
//
// Threads: OS callbacks run on the OS task (LVGL locked). Emulation runs in
// its own FreeRTOS task at idle priority so the launcher, LVGL and radios
// always preempt it; the task yields every frame. A prio-4 blit helper sends
// packed 240x216 frames to the ILI9341; Peanut-GB only copies, it does not
// wait on SPI. Background radios and services (Wi-Fi, ESP-NOW mesh, cloud,
// game tick, Lua host, BLE) pause for the ROM and restore on the list, HOME,
// or exit — same as GBA.
//
// Screen: 160x144 scaled 1.5x to 240x216, centred on the 320x240 ILI9341
// (direct panel blit, not an LVGL image). LVGL's refresh timer is paused
// while a ROM runs; the status bar is a battery tick every 5 s.
//
// Button chords (6 front keys + BOOT on the back; calibrated os_key map):
//   d-pad                 GB d-pad
//   SELECT                A
//   CANCEL                B
//   UP+DOWN               START (latched; d-pad swallowed until both release)
//   SELECT+CANCEL         START (second mapping; does not steal HOME)
//   LEFT+RIGHT            SELECT (GB)
//   SELECT+START          exit to ROM list  (badge SELECT + UP+DOWN)
//   SELECT+A              write battery .sav now  (GB SELECT + A:
//                         LEFT+RIGHT + badge SELECT)
//   BOOT (HOME)           OS exit; cart RAM is saved
//   console `key start` injects GB START for 350 ms
// Battery SRAM is written to <rom>.sav next to the ROM (same basename), on
// those chords, ~1 s after the game last wrote cart RAM, every minute while
// dirty, and when leaving.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"
#include "apps/emu_pad.h"

#define ENABLE_SOUND                0
#define ENABLE_LCD                  1
#define PEANUT_GB_IS_LITTLE_ENDIAN  1
#define PEANUT_GB_HEADER_ONLY
#include "peanut_gb.h"

static const char *TAG = "gbemu";

#define SCALE_W         240
#define SCALE_H         216
#define FRAME_US        16743
#define SAVE_EVERY_US   (60 * 1000000LL)
#define SAVE_IDLE_US    (1 * 1000000LL)
#define ROM_MAX         (4u << 20)
#define MAX_ROMS        32
#define PATH_MAX_LEN    200
#define CHORD_MS        250
#define ROMS_DIR        BSP_SD_MOUNT "/roms"
#define BLIT_TASK_STACK 3072
#define BLIT_TASK_PRIO  4
#define GB_BLIT_BUFS    3
#define GB_BLIT_X       ((BOARD_LCD_H_RES - SCALE_W) / 2)
#define GB_BLIT_Y       (OS_STATUS_H + (OS_ROOT_H - SCALE_H) / 2)
#define GB_FB_BYTES     ((size_t)SCALE_W * (size_t)SCALE_H * sizeof(uint16_t))

static const uint32_t PALETTE_HEX[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};

typedef struct {
    struct gb_s gb;
    uint8_t *rom;
    size_t rom_size;
    uint8_t *ram;
    size_t ram_size;
    bool ram_dirty;
    int64_t ram_touch_us;
    uint16_t pal[4];
    uint16_t *fb;
    bool drawn;
    char rom_path[PATH_MAX_LEN];
    char sav_path[PATH_MAX_LEN];
} emu_t;

static emu_t *s_emu;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static volatile bool s_back_to_list;
static int64_t s_exit_since, s_save_since;
static bool s_save_armed = true;
static lv_obj_t *s_root, *s_msg, *s_hint;
static uint16_t *s_blit_fb[GB_BLIT_BUFS];
static TaskHandle_t s_blit_task;
static SemaphoreHandle_t s_blit_kick;
static SemaphoreHandle_t s_blit_done;
static portMUX_TYPE s_blit_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_blit_pending = -1;
static volatile int s_blit_sending = -1;
static volatile int s_draw;
static volatile bool s_blit_quit;
static ui_list_t *s_list;
static char s_roms[MAX_ROMS][PATH_MAX_LEN];
static int s_rom_count;
static emu_pad_t s_pad;

/* Internal-SRAM copy of GB ROM bank 0 so rom_read does not hit PSRAM for
 * 0000–3FFF. Two 16 KB windows do not fit: heap malloc after pause cannot
 * get 16 KB contiguous, and 32 KB BSS drops boot heap to 102 KiB and
 * abort()s in the launcher. A 16 KB window on the switchable bank also
 * loses (Yellow recopies 16 KB from PSRAM on every MBC switch). Bank 0 is
 * copied once. */
static uint8_t s_rom_win0[ROM_BANK_SIZE];
static uint8_t *s_rom_win[2];
static int s_rom_win_n;
static uint32_t s_win1_base = UINT32_MAX;

static IRAM_ATTR void rom_win_load(uint8_t *dst, const uint8_t *rom, size_t rom_size,
                                   uint32_t base)
{
    size_t n = ROM_BANK_SIZE;
    if (base >= rom_size) {
        memset(dst, 0xFF, ROM_BANK_SIZE);
        return;
    }
    if (rom_size - base < n) {
        n = rom_size - base;
    }
    memcpy(dst, rom + base, n);
    if (n < ROM_BANK_SIZE) {
        memset(dst + n, 0xFF, ROM_BANK_SIZE - n);
    }
}

static void rom_win_free(void)
{
    s_rom_win_n = 0;
    s_rom_win[0] = s_rom_win[1] = NULL;
    s_win1_base = UINT32_MAX;
}

static void rom_win_alloc(emu_t *e)
{
    s_rom_win[0] = s_rom_win0;
    s_rom_win[1] = NULL;
    s_rom_win_n = 1;
    s_win1_base = UINT32_MAX;
    rom_win_load(s_rom_win[0], e->rom, e->rom_size, 0);
    ESP_LOGI(TAG, "ROM SRAM window: 16 KB BSS (bank 0; 32 KB does not fit)");
}

static IRAM_ATTR uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    if (addr >= e->rom_size) {
        return 0xFF;
    }
    if (s_rom_win[0] && addr < ROM_BANK_SIZE) {
        return s_rom_win[0][addr];
    }
    if (s_rom_win[1] && addr >= ROM_BANK_SIZE) {
        uint32_t base = addr & ~(uint32_t)(ROM_BANK_SIZE - 1);
        if (base != s_win1_base) {
            rom_win_load(s_rom_win[1], e->rom, e->rom_size, base);
            s_win1_base = base;
        }
        return s_rom_win[1][addr - base];
    }
    return e->rom[addr];
}

static IRAM_ATTR uint8_t ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    return addr < e->ram_size ? e->ram[addr] : 0xFF;
}

static IRAM_ATTR void ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    emu_t *e = gb->direct.priv;
    if (addr < e->ram_size && e->ram[addr] != val) {
        e->ram[addr] = val;
        e->ram_dirty = true;
        e->ram_touch_us = esp_timer_get_time();
    }
}

static void gb_err(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    (void)gb;
    ESP_LOGE(TAG, "emulation error %d at 0x%04X", (int)err, addr);
    os_toast("Game Boy error %d at %04X", (int)err, addr);
    s_quit = true;
}

// Line y of 144 into rows of the 240x216 back buffer: even lines fill two
// rows, odd lines one. Columns: 2 pixels -> 3.
static IRAM_ATTR void draw_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t y)
{
    emu_t *e = gb->direct.priv;
    int row = (y & 1) ? (3 * y + 1) / 2 : 3 * y / 2;
    uint16_t *dst = e->fb + row * SCALE_W;
    for (int x = 0; x < LCD_WIDTH; x += 2) {
        uint16_t a = e->pal[px[x] & 3], b = e->pal[px[x + 1] & 3];
        *dst++ = a;
        *dst++ = a;
        *dst++ = b;
    }
    if (!(y & 1)) {
        memcpy(e->fb + (row + 1) * SCALE_W, e->fb + row * SCALE_W, SCALE_W * sizeof(uint16_t));
    }
    if (y == LCD_HEIGHT - 1) {
        e->drawn = true;
    }
}

static bool load_file(const char *path, uint8_t **out, size_t *size, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && (size_t)n <= max;
    uint8_t *buf = ok ? heap_caps_malloc((size_t)n, os_mem_bulk_caps()) : NULL;
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

static void sav_from_rom(char *sav, size_t n, const char *rom)
{
    strlcpy(sav, rom, n);
    char *slash = strrchr(sav, '/');
    char *dot = strrchr(slash ? slash + 1 : sav, '.');
    if (dot) {
        strlcpy(dot, ".sav", n - (size_t)(dot - sav));
    } else {
        strlcat(sav, ".sav", n);
    }
}

static void save_ram(emu_t *e, bool toast)
{
    if (!e->ram_size || !e->ram_dirty) {
        return;
    }
    FILE *f = fopen(e->sav_path, "wb");
    if (f && fwrite(e->ram, 1, e->ram_size, f) == e->ram_size) {
        e->ram_dirty = false;
        ESP_LOGI(TAG, "saved %u B to %s", (unsigned)e->ram_size, e->sav_path);
        if (toast) {
            os_toast("Saved");
        }
    } else {
        ESP_LOGW(TAG, "save to %s failed", e->sav_path);
        if (toast) {
            os_toast("Save failed");
        }
    }
    if (f) {
        fclose(f);
    }
}

static uint8_t joypad(void)
{
    uint32_t held = emu_pad_joy(&s_pad);
    emu_pad_log(&s_pad, held, TAG);
    return (uint8_t)~held;
}

static void chords(emu_t *e, int64_t now)
{
    if (s_pad.exit_chord) {
        if (!s_exit_since) {
            s_exit_since = now;
        } else if (now - s_exit_since >= CHORD_MS * 1000LL) {
            s_back_to_list = true;
            s_quit = true;
        }
    } else {
        s_exit_since = 0;
    }

    if (s_pad.save_chord) {
        if (s_save_armed && (!s_save_since || now - s_save_since >= CHORD_MS * 1000LL)) {
            if (e->ram_size) {
                e->ram_dirty = true;
                save_ram(e, true);
            } else {
                os_toast("This game has no save RAM");
            }
            s_save_since = now;
            s_save_armed = false;
        }
    } else {
        s_save_armed = true;
        s_save_since = 0;
    }
}

static void blit_task(void *arg)
{
    (void)arg;
    while (!s_blit_quit) {
        if (xSemaphoreTake(s_blit_kick, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        for (;;) {
            portENTER_CRITICAL(&s_blit_mux);
            int i = s_blit_pending;
            if (i < 0 || i == s_draw) {
                portEXIT_CRITICAL(&s_blit_mux);
                break;
            }
            s_blit_pending = -1;
            s_blit_sending = i;
            portEXIT_CRITICAL(&s_blit_mux);
            (void)bsp_lcd_blit_rgb565(GB_BLIT_X, GB_BLIT_Y, SCALE_W, SCALE_H,
                                      s_blit_fb[i], SCALE_W);
            portENTER_CRITICAL(&s_blit_mux);
            s_blit_sending = -1;
            portEXIT_CRITICAL(&s_blit_mux);
        }
    }
    if (s_blit_done) {
        xSemaphoreGive(s_blit_done);
    }
    vTaskDelete(NULL);
}

static bool present(emu_t *e)
{
    if (s_quit || !e || !e->fb) {
        return false;
    }
    if (!s_blit_fb[0] || !s_blit_kick) {
        return bsp_lcd_blit_rgb565(GB_BLIT_X, GB_BLIT_Y, SCALE_W, SCALE_H,
                                   e->fb, SCALE_W) == ESP_OK;
    }

    portENTER_CRITICAL(&s_blit_mux);
    int i = s_draw;
    s_blit_pending = i;
    int next = i;
    for (int n = 0; n < GB_BLIT_BUFS; n++) {
        if (n != i && n != s_blit_sending) {
            next = n;
            break;
        }
    }
    s_draw = next;
    e->fb = s_blit_fb[next];
    portEXIT_CRITICAL(&s_blit_mux);
    xSemaphoreGive(s_blit_kick);
    return true;
}

static void blit_stop(void)
{
    if (s_blit_task) {
        s_blit_quit = true;
        if (s_blit_kick) {
            xSemaphoreGive(s_blit_kick);
        }
        if (s_blit_done) {
            (void)xSemaphoreTake(s_blit_done, pdMS_TO_TICKS(2000));
        }
        s_blit_task = NULL;
    }
    if (s_blit_kick) {
        vSemaphoreDelete(s_blit_kick);
        s_blit_kick = NULL;
    }
    if (s_blit_done) {
        vSemaphoreDelete(s_blit_done);
        s_blit_done = NULL;
    }
    for (int i = 0; i < GB_BLIT_BUFS; i++) {
        heap_caps_free(s_blit_fb[i]);
        s_blit_fb[i] = NULL;
    }
    s_blit_pending = s_blit_sending = -1;
    s_draw = 0;
}

static bool blit_start(void)
{
    blit_stop();
    for (int i = 0; i < GB_BLIT_BUFS; i++) {
        s_blit_fb[i] = heap_caps_malloc(GB_FB_BYTES, os_mem_bulk_caps());
    }
    s_blit_kick = xSemaphoreCreateBinary();
    s_blit_done = xSemaphoreCreateBinary();
    s_blit_quit = false;
    s_blit_pending = s_blit_sending = -1;
    s_draw = 0;
    if (!s_blit_fb[0] || !s_blit_fb[1] || !s_blit_fb[2] || !s_blit_kick || !s_blit_done ||
        xTaskCreate(blit_task, "gbblit", BLIT_TASK_STACK, NULL,
                    BLIT_TASK_PRIO, &s_blit_task) != pdPASS) {
        ESP_LOGW(TAG, "async blit unavailable; SPI waits on the emu task");
        blit_stop();
        return false;
    }
    return true;
}

static void play_video(bool on)
{
    if (on) {
        lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        lv_obj_set_hidden(s_root, false);
        lv_refr_now(NULL);
        lv_obj_set_hidden(s_root, true);
        bsp_display_pause_lvgl(true);
        blit_start();
    } else {
        blit_stop();
        bsp_display_pause_lvgl(false);
        if (s_root) {
            lv_obj_set_hidden(s_root, false);
        }
    }
}

static void free_emu(emu_t *e)
{
    rom_win_free();
    if (!e) {
        return;
    }
    heap_caps_free(e->rom);
    heap_caps_free(e->ram);
    if (e->fb && e->fb != s_blit_fb[0] && e->fb != s_blit_fb[1] &&
            e->fb != s_blit_fb[2]) {
        heap_caps_free(e->fb);
    }
    e->fb = NULL;
    heap_caps_free(e);
}

static void show_picker(void);

static void back_to_list(void *arg)
{
    (void)arg;
    if (s_task) {
        s_quit = true;
        if (s_done && xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "emulator task did not stop; leaking it");
            s_emu = NULL;
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    free_emu(s_emu);
    s_emu = NULL;
    s_back_to_list = false;
    os_background_unpause();
    play_video(false);
    if (s_root) {
        lv_obj_clean(s_root);
        show_picker();
    }
}

static void emu_task(void *arg)
{
    (void)arg;
    emu_t *e = s_emu;
    if (!load_file(e->rom_path, &e->rom, &e->rom_size, ROM_MAX)) {
        os_toast("Can't read that ROM (max 4 MB)");
        goto out;
    }
    enum gb_init_error_e ie = gb_init(&e->gb, rom_read, ram_read, ram_write, gb_err, e);
    if (ie != GB_INIT_NO_ERROR) {
        os_toast(ie == GB_INIT_CARTRIDGE_UNSUPPORTED ? "Cartridge type not supported"
                                                    : "ROM header checksum is wrong");
        goto out;
    }
    size_t want = 0;
    gb_get_save_size_s(&e->gb, &want);
    if (want) {
        uint8_t *sav = NULL;
        size_t got = 0;
        if (load_file(e->sav_path, &sav, &got, want) && got == want) {
            e->ram = sav;
        } else {
            heap_caps_free(sav);
            e->ram = heap_caps_calloc(1, want, os_mem_bulk_caps());
        }
        if (!e->ram) {
            os_toast("Out of memory for save RAM");
            goto out;
        }
        e->ram_size = want;
    }
    gb_init_lcd(&e->gb, draw_line);
    e->gb.direct.frame_skip = true;
    s_exit_since = s_save_since = 0;
    s_save_armed = true;
    emu_pad_reset(&s_pad);
    char title[17];
    ESP_LOGI(TAG, "%s: %u KB ROM, %u B cart RAM", gb_get_rom_name(&e->gb, title),
             (unsigned)(e->rom_size >> 10), (unsigned)e->ram_size);
    os_background_pause();
    rom_win_alloc(e);

    int64_t next = esp_timer_get_time(), last_save = next, stat_t = next;
    int frames = 0, shown = 0, late = 0;
    while (!s_quit) {
        int64_t now = esp_timer_get_time();
        emu_pad_sample(&s_pad);
        chords(e, now);
        if (s_quit) {
            break;
        }
        e->gb.direct.joypad = joypad();
        gb_run_frame(&e->gb);
        if (e->drawn) {
            e->drawn = false;
            shown += present(e);
        }
        now = esp_timer_get_time();
        next += FRAME_US;
        if (next - now >= 10000) {
            vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        } else {
            if (now - next > 100000) {
                next = now;
                late++;
            }
            vTaskDelay(0);
        }
        frames++;
        if (now - stat_t >= 5000000) {
            double s = (now - stat_t) / 1e6;
            ESP_LOGI(TAG, "%.1f emulated fps (target 59.7), %.1f shown, %d resyncs",
                     frames / s, shown / s, late);
            stat_t = now;
            frames = shown = late = 0;
        }
        if (e->ram_dirty && e->ram_touch_us && now - e->ram_touch_us >= SAVE_IDLE_US) {
            save_ram(e, false);
            last_save = now;
        } else if (e->ram_dirty && now - last_save >= SAVE_EVERY_US) {
            save_ram(e, false);
            last_save = now;
        }
    }
    save_ram(e, false);
out:
    xSemaphoreGive(s_done);
    if (s_back_to_list) {
        os_post(back_to_list, NULL);
    }
    vTaskDelete(NULL);
}

static bool is_rom(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

static void scan(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && s_rom_count < MAX_ROMS) {
        size_t dl = strlen(dir), nl = strlen(de->d_name);
        if (is_rom(de->d_name) && dl + 1 + nl < PATH_MAX_LEN) {
            char *p = s_roms[s_rom_count++];
            memcpy(p, dir, dl);
            p[dl] = '/';
            memcpy(p + dl + 1, de->d_name, nl + 1);
            ESP_LOGI(TAG, "rom %d %s", s_rom_count - 1, de->d_name);
        }
    }
    closedir(d);
}

static void rom_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    const char *slash = strrchr(s_roms[i], '/');
    strlcpy(row->title, slash ? slash + 1 : s_roms[i], sizeof(row->title));
    strlcpy(row->sub, s_roms[i], sizeof(row->sub));
}

static void message(const char *text)
{
    s_msg = ui_label(s_root, &sol_font_14, COL_TEXT, text);
    lv_obj_set_width(s_msg, OS_ROOT_W - 32);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, -8);
}

static void show_picker(void)
{
    s_list = NULL;
    s_msg = s_hint = NULL;
    s_rom_count = 0;

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nInsert one with a .gb file in /roms.");
        return;
    }
    scan(ROMS_DIR);
    if (!s_rom_count) {
        message("No ROM found.\nCopy a .gb or .gbc file to /roms on the card.");
        return;
    }
    s_list = ui_list_create(s_root, 0, OS_ROOT_H - UI_HINT_H, rom_row, NULL);
    ui_list_set_count(s_list, s_rom_count);
    s_hint = ui_hint(s_root, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT play   CANCEL exit");
}

static void start(const char *path)
{
    emu_t *e = heap_caps_calloc(1, sizeof(*e), os_mem_bulk_caps());
    if (!e) {
        os_toast("Out of memory");
        return;
    }
    strlcpy(e->rom_path, path, sizeof(e->rom_path));
    sav_from_rom(e->sav_path, sizeof(e->sav_path), path);
    for (int i = 0; i < 4; i++) {
        e->pal[i] = lv_color_to_u16(lv_color_hex(PALETTE_HEX[i]));
    }

    if (s_list) {
        ui_list_show(s_list, false);
        s_list = NULL;
    }
    if (s_hint) {
        lv_obj_delete(s_hint);
        s_hint = NULL;
    }
    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    play_video(true);
    if (s_blit_fb[0]) {
        e->fb = s_blit_fb[0];
        s_draw = 0;
    } else {
        e->fb = heap_caps_calloc(SCALE_W * SCALE_H, sizeof(uint16_t), os_mem_bulk_caps());
        if (!e->fb) {
            free_emu(e);
            play_video(false);
            os_toast("Out of memory");
            lv_obj_clean(s_root);
            show_picker();
            return;
        }
    }
    for (int i = 0; i < SCALE_W * SCALE_H; i++) {
        e->fb[i] = e->pal[0];
    }

    s_emu = e;
    s_quit = false;
    s_back_to_list = false;
    s_done = xSemaphoreCreateBinary();
    if (!s_done || xTaskCreate(emu_task, "gbemu", 6144, NULL, tskIDLE_PRIORITY, &s_task) != pdPASS) {
        os_toast("Can't start the emulator task");
        if (s_done) {
            vSemaphoreDelete(s_done);
            s_done = NULL;
        }
        free_emu(e);
        s_emu = NULL;
        s_task = NULL;
        play_video(false);
        lv_obj_clean(s_root);
        show_picker();
        return;
    }
    os_toast("UP+DOWN or SELECT+CANCEL = Start");
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_task = NULL;
    s_emu = NULL;
    s_done = NULL;
    s_quit = false;
    s_back_to_list = false;
    show_picker();
}

static void leave(void)
{
    s_back_to_list = false;
    if (s_task) {
        s_quit = true;
        if (s_done && xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "emulator task did not stop; leaking it");
            s_emu = NULL;
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    free_emu(s_emu);
    s_emu = NULL;
    os_background_unpause();
    play_video(false);
    s_root = NULL;
}

static bool key(os_key_t k, bool repeat)
{
    if (s_task) {
        return true;
    }
    if (s_list && k == OS_KEY_SELECT && !repeat) {
        int i = ui_list_selected(s_list);
        if (i >= 0) {
            start(s_roms[i]);
        }
        return true;
    }
    return s_list && ui_list_key(s_list, k, repeat);
}

const os_app_t app_gbemu = {
    .id = "gbemu",
    .name = "Game Boy",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
};
