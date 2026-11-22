// Game Boy: plays a .gb / .gbc ROM from the microSD card (/sdcard/roms).
//
// The CPU/PPU core is Peanut-GB (peanut_gb.h, MIT, vendored next to this file).
// No ROM ships with the firmware: copy a legally owned dump onto the card.
//
// Threads: OS callbacks run on the OS task (LVGL locked). Emulation runs in
// its own FreeRTOS task at idle priority so the launcher, LVGL and radios
// always preempt it; the task yields every frame. Wi-Fi is paused (driver
// stopped) while a ROM is running and restored when play ends.
//
// Screen: 160x144 scaled 1.5x to 240x216, centred in the 320x218 app root
// (status bar keeps the panel at 320x240). Each frame lands in one of two
// RGB565 buffers; the task swaps the image source under the LVGL lock.
//
// Button chords (6 front keys + BOOT on the back; calibrated os_key map):
//   d-pad                 GB d-pad
//   SELECT                A
//   CANCEL                B
//   UP+DOWN               START
//   LEFT+RIGHT            SELECT (GB)
//   SELECT+START          exit to ROM list  (badge SELECT + UP+DOWN)
//   SELECT+A              write battery .sav now  (GB SELECT + A:
//                         LEFT+RIGHT + badge SELECT)
//   BOOT (HOME)           OS exit; cart RAM is saved
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
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "core/os_wifi.h"
#include "core/ui.h"

#define ENABLE_SOUND                0
#define ENABLE_LCD                  1
#define PEANUT_GB_IS_LITTLE_ENDIAN  1
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
#define SRAM_KEEP       (64 * 1024)
#define LOCK_WAIT_MS    4
#define CHORD_MS        250
#define ROMS_DIR        BSP_SD_MOUNT "/roms"

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
    uint16_t *buf[2];
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
static lv_image_dsc_t s_dsc[2];
static lv_obj_t *s_root, *s_img, *s_msg, *s_hint;
static ui_list_t *s_list;
static char s_roms[MAX_ROMS][PATH_MAX_LEN];
static int s_rom_count;

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    return addr < e->rom_size ? e->rom[addr] : 0xFF;
}

static uint8_t ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    return addr < e->ram_size ? e->ram[addr] : 0xFF;
}

static void ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
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
static void draw_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t y)
{
    emu_t *e = gb->direct.priv;
    int row = (y & 1) ? (3 * y + 1) / 2 : 3 * y / 2;
    uint16_t *dst = e->buf[1] + row * SCALE_W;
    for (int x = 0; x < LCD_WIDTH; x += 2) {
        uint16_t a = e->pal[px[x] & 3], b = e->pal[px[x + 1] & 3];
        *dst++ = a;
        *dst++ = a;
        *dst++ = b;
    }
    if (!(y & 1)) {
        memcpy(e->buf[1] + (row + 1) * SCALE_W, e->buf[1] + row * SCALE_W, SCALE_W * sizeof(uint16_t));
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
    uint32_t b = bsp_buttons_get();
    bool up = b & os_key_button(OS_KEY_UP), down = b & os_key_button(OS_KEY_DOWN);
    bool left = b & os_key_button(OS_KEY_LEFT), right = b & os_key_button(OS_KEY_RIGHT);
    bool a = b & os_key_button(OS_KEY_SELECT);
    bool start = up && down;
    bool gb_select = left && right;
    uint8_t held = 0;

    // SELECT+START (badge SELECT + UP+DOWN): leave the game, not A+START.
    if (a && start) {
        return 0xFF;
    }
    // SELECT+A (GB SELECT + A): force-save; don't also feed SELECT+A to the game.
    if (a && gb_select) {
        return 0xFF;
    }

    if (start) {
        held |= JOYPAD_START;
    } else {
        held |= (up ? JOYPAD_UP : 0) | (down ? JOYPAD_DOWN : 0);
    }
    if (gb_select) {
        held |= JOYPAD_SELECT;
    } else {
        held |= (left ? JOYPAD_LEFT : 0) | (right ? JOYPAD_RIGHT : 0);
    }
    if (a) {
        held |= JOYPAD_A;
    }
    if (b & os_key_button(OS_KEY_BACK)) {
        held |= JOYPAD_B;
    }
    return (uint8_t)~held;
}

static void chords(emu_t *e, int64_t now)
{
    uint32_t b = bsp_buttons_get();
    bool a = b & os_key_button(OS_KEY_SELECT);
    bool start = (b & os_key_button(OS_KEY_UP)) && (b & os_key_button(OS_KEY_DOWN));
    bool gb_select = (b & os_key_button(OS_KEY_LEFT)) && (b & os_key_button(OS_KEY_RIGHT));

    if (a && start) {
        if (!s_exit_since) {
            s_exit_since = now;
        } else if (now - s_exit_since >= CHORD_MS * 1000LL) {
            s_back_to_list = true;
            s_quit = true;
        }
    } else {
        s_exit_since = 0;
    }

    if (a && gb_select) {
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

static bool present(emu_t *e)
{
    if (!lvgl_port_lock(LOCK_WAIT_MS)) {
        return false;
    }
    if (!s_quit && s_img) {
        uint16_t *t = e->buf[0];
        e->buf[0] = e->buf[1];
        e->buf[1] = t;
        lv_image_dsc_t *d = s_dsc[0].data == (const uint8_t *)e->buf[0] ? &s_dsc[0] : &s_dsc[1];
        lv_image_set_src(s_img, d);
        lv_obj_invalidate(s_img);
    }
    lvgl_port_unlock();
    return true;
}

static void free_emu(emu_t *e)
{
    if (!e) {
        return;
    }
    heap_caps_free(e->rom);
    heap_caps_free(e->ram);
    heap_caps_free(e->buf[0]);
    heap_caps_free(e->buf[1]);
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
    s_img = NULL;
    s_back_to_list = false;
    os_wifi_unpause();
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
    char title[17];
    ESP_LOGI(TAG, "%s: %u KB ROM, %u B cart RAM", gb_get_rom_name(&e->gb, title),
             (unsigned)(e->rom_size >> 10), (unsigned)e->ram_size);
    os_wifi_pause();

    int64_t next = esp_timer_get_time(), last_save = next, stat_t = next;
    int frames = 0, shown = 0, late = 0;
    while (!s_quit) {
        int64_t now = esp_timer_get_time();
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
    s_msg = s_img = s_hint = NULL;
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
    emu_t *e = NULL;
    if (heap_caps_get_free_size(OS_MEM_SRAM_MALLOC) > sizeof(*e) + SRAM_KEEP) {
        e = heap_caps_calloc(1, sizeof(*e), OS_MEM_SRAM);
    }
    if (!e) {
        e = heap_caps_calloc(1, sizeof(*e), os_mem_bulk_caps());
    }
    for (int i = 0; e && i < 2; i++) {
        e->buf[i] = heap_caps_calloc(SCALE_W * SCALE_H, sizeof(uint16_t), os_mem_bulk_caps());
        if (!e->buf[i]) {
            free_emu(e);
            e = NULL;
        }
    }
    if (!e) {
        os_toast("Out of memory");
        return;
    }
    strlcpy(e->rom_path, path, sizeof(e->rom_path));
    sav_from_rom(e->sav_path, sizeof(e->sav_path), path);
    for (int i = 0; i < 4; i++) {
        e->pal[i] = lv_color_to_u16(lv_color_hex(PALETTE_HEX[i]));
    }
    for (int i = 0; i < SCALE_W * SCALE_H; i++) {
        e->buf[0][i] = e->pal[0];
    }
    for (int i = 0; i < 2; i++) {
        s_dsc[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = SCALE_W, .h = SCALE_H, .stride = SCALE_W * 2},
            .data_size = SCALE_W * SCALE_H * 2,
            .data = (const uint8_t *)e->buf[i],
        };
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
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    s_img = lv_image_create(s_root);
    lv_image_set_src(s_img, &s_dsc[0]);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

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
        if (s_img) {
            lv_obj_delete(s_img);
            s_img = NULL;
        }
        free_emu(e);
        s_emu = NULL;
        s_task = NULL;
        lv_obj_clean(s_root);
        show_picker();
        return;
    }
    os_toast("BOOT / SELECT+START exit  " LV_SYMBOL_BULLET "  SELECT+A save");
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
    s_img = NULL;
    s_root = NULL;
    os_wifi_unpause();
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
