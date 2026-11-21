// Game Boy Advance: plays a .gba ROM from the microSD card (/sdcard/roms).
//
// The CPU/PPU core is 44VBA (vba-next fork, GPL-3), vendored under vendor/44vba.
// No ROM ships with the firmware: copy a legally owned dump onto the card.
//
// Threads: OS callbacks run on the OS task (LVGL locked). Emulation runs in
// its own FreeRTOS task at idle priority so the launcher, LVGL and radios
// always preempt it; the task yields every frame.
//
// Screen: native 240x160 centred in the 320x218 app root (status bar keeps
// the panel at 320x240). Each frame lands in a 240x160 RGB565 buffer; the
// task updates the image source under the LVGL lock.
//
// Button chords (6 front keys + BOOT on the back; calibrated os_key map):
//   d-pad                 GBA d-pad
//   SELECT                A
//   CANCEL                B
//   UP+DOWN               START
//   LEFT+RIGHT            SELECT
//   SELECT+START          exit to ROM list  (badge SELECT + UP+DOWN)
//   SELECT+A              write battery .sav now  (GBA SELECT + A:
//                         LEFT+RIGHT + badge SELECT)
//   BOOT (HOME)           OS exit; save RAM is written
// L and R are not mapped (the badge has no shoulder buttons).
// Battery SRAM/Flash/EEPROM is written to <rom>.sav next to the ROM (same
// basename), on those chords, when the save buffer changes, and on quit.

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
#include "core/ui.h"
#include "gbaemu_host.h"

static const char *TAG = "gbaemu";

#define FRAME_US        16743
#define SAVE_EVERY_US   (60 * 1000000LL)
#define SAVE_IDLE_US    (2 * 1000000LL)
#define MAX_ROMS        32
#define PATH_MAX_LEN    200
#define LOCK_WAIT_MS    4
#define CHORD_MS        250
#define ROMS_DIR        BSP_SD_MOUNT "/roms"
#define TASK_STACK      16384

// GBA P1 bits (44VBA / vba-next joy):
#define GBA_A       (1u << 0)
#define GBA_B       (1u << 1)
#define GBA_SELECT  (1u << 2)
#define GBA_START   (1u << 3)
#define GBA_RIGHT   (1u << 4)
#define GBA_LEFT    (1u << 5)
#define GBA_UP      (1u << 6)
#define GBA_DOWN    (1u << 7)
#define GBA_R       (1u << 8)
#define GBA_L       (1u << 9)

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static volatile bool s_back_to_list;
static int64_t s_exit_since, s_save_since;
static bool s_save_armed = true;
static lv_image_dsc_t s_dsc;
static lv_obj_t *s_root, *s_img, *s_msg, *s_hint;
static ui_list_t *s_list;
static char s_roms[MAX_ROMS][PATH_MAX_LEN];
static char s_sav_path[PATH_MAX_LEN];
static int s_rom_count;
static uint16_t *s_blank;

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

static uint32_t joypad(void)
{
    uint32_t b = bsp_buttons_get();
    bool up = b & os_key_button(OS_KEY_UP), down = b & os_key_button(OS_KEY_DOWN);
    bool left = b & os_key_button(OS_KEY_LEFT), right = b & os_key_button(OS_KEY_RIGHT);
    bool a = b & os_key_button(OS_KEY_SELECT);
    bool start = up && down;
    bool gba_select = left && right;
    uint32_t held = 0;

    if (a && start) {
        return 0;
    }
    if (a && gba_select) {
        return 0;
    }

    if (start) {
        held |= GBA_START;
    } else {
        held |= (up ? GBA_UP : 0) | (down ? GBA_DOWN : 0);
    }
    if (gba_select) {
        held |= GBA_SELECT;
    } else {
        held |= (left ? GBA_LEFT : 0) | (right ? GBA_RIGHT : 0);
    }
    if (a) {
        held |= GBA_A;
    }
    if (b & os_key_button(OS_KEY_BACK)) {
        held |= GBA_B;
    }
    return held;
}

static void chords(int64_t now)
{
    uint32_t b = bsp_buttons_get();
    bool a = b & os_key_button(OS_KEY_SELECT);
    bool start = (b & os_key_button(OS_KEY_UP)) && (b & os_key_button(OS_KEY_DOWN));
    bool gba_select = (b & os_key_button(OS_KEY_LEFT)) && (b & os_key_button(OS_KEY_RIGHT));

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

    if (a && gba_select) {
        if (s_save_armed && (!s_save_since || now - s_save_since >= CHORD_MS * 1000LL)) {
            gba_core_save(s_sav_path, true);
            s_save_since = now;
            s_save_armed = false;
        }
    } else {
        s_save_armed = true;
        s_save_since = 0;
    }
}

static bool present(const uint16_t *fb)
{
    if (!lvgl_port_lock(LOCK_WAIT_MS)) {
        return false;
    }
    if (!s_quit && s_img && fb) {
        s_dsc.data = (const uint8_t *)fb;
        lv_image_set_src(s_img, &s_dsc);
        lv_obj_invalidate(s_img);
    }
    lvgl_port_unlock();
    return true;
}

static void show_picker(void);

static void back_to_list(void *arg)
{
    (void)arg;
    if (s_task) {
        s_quit = true;
        if (s_done && xSemaphoreTake(s_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
            ESP_LOGW(TAG, "emulator task did not stop; leaking it");
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    gba_core_halt();
    heap_caps_free(s_blank);
    s_blank = NULL;
    s_img = NULL;
    s_back_to_list = false;
    if (s_root) {
        lv_obj_clean(s_root);
        show_picker();
    }
}

static void emu_task(void *arg)
{
    const char *path = (const char *)arg;
    sav_from_rom(s_sav_path, sizeof(s_sav_path), path);
    if (!gba_core_boot(path, s_sav_path)) {
        s_back_to_list = true;
        goto out;
    }
    s_exit_since = s_save_since = 0;
    s_save_armed = true;

    int64_t next = esp_timer_get_time(), last_save = next, stat_t = next, last_touch = next;
    int frames = 0, shown = 0, late = 0;
    while (!s_quit) {
        int64_t now = esp_timer_get_time();
        chords(now);
        if (s_quit) {
            break;
        }
        const uint16_t *fb = NULL;
        if (gba_core_frame(joypad(), &fb)) {
            shown += present(fb);
            if (gba_core_save_dirty()) {
                last_touch = now;
            }
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
            vTaskDelay(1);
        }
        frames++;
        if (now - stat_t >= 5000000) {
            double s = (now - stat_t) / 1e6;
            ESP_LOGI(TAG, "%.1f emulated fps (target ~30 with skip), %.1f shown, %d resyncs",
                     frames / s, shown / s, late);
            stat_t = now;
            frames = shown = late = 0;
        }
        if (gba_core_save_dirty() && now - last_touch >= SAVE_IDLE_US) {
            gba_core_save(s_sav_path, false);
            last_save = now;
        } else if (gba_core_save_dirty() && now - last_save >= SAVE_EVERY_US) {
            gba_core_save(s_sav_path, false);
            last_save = now;
        }
    }
    gba_core_save(s_sav_path, false);
out:
    gba_core_halt();
    xSemaphoreGive(s_done);
    if (s_back_to_list) {
        os_post(back_to_list, NULL);
    }
    vTaskDelete(NULL);
}

static bool is_rom(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".gba");
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
        message("No microSD card.\nInsert one with a .gba file in /roms.");
        return;
    }
    scan(ROMS_DIR);
    if (!s_rom_count) {
        message("No ROM found.\nCopy a .gba file to /roms on the card.");
        return;
    }
    s_list = ui_list_create(s_root, 0, OS_ROOT_H - UI_HINT_H, rom_row, NULL);
    ui_list_set_count(s_list, s_rom_count);
    s_hint = ui_hint(s_root, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT play   CANCEL exit");
}

static void start(const char *path)
{
#ifdef SOLANA_OS_EMULATOR
    os_toast("GBA runs on the badge, not the web emulator");
    return;
#else
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

    if (!s_blank) {
        s_blank = heap_caps_calloc(GBA_WIDTH * GBA_HEIGHT, sizeof(uint16_t), os_mem_bulk_caps());
    }
    if (!s_blank) {
        os_toast("Out of memory");
        lv_obj_clean(s_root);
        show_picker();
        return;
    }
    s_dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = GBA_WIDTH, .h = GBA_HEIGHT, .stride = GBA_WIDTH * 2},
        .data_size = GBA_WIDTH * GBA_HEIGHT * 2,
        .data = (const uint8_t *)s_blank,
    };
    s_img = lv_image_create(s_root);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

    s_quit = false;
    s_back_to_list = false;
    s_done = xSemaphoreCreateBinary();
    if (!s_done || xTaskCreate(emu_task, "gbaemu", TASK_STACK, (void *)path,
                               tskIDLE_PRIORITY, &s_task) != pdPASS) {
        os_toast("Can't start the emulator task");
        if (s_done) {
            vSemaphoreDelete(s_done);
            s_done = NULL;
        }
        if (s_img) {
            lv_obj_delete(s_img);
            s_img = NULL;
        }
        s_task = NULL;
        lv_obj_clean(s_root);
        show_picker();
        return;
    }
    os_toast("BOOT / SELECT+START exit  " LV_SYMBOL_BULLET "  SELECT+A save");
#endif
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_task = NULL;
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
        if (s_done && xSemaphoreTake(s_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
            ESP_LOGW(TAG, "emulator task did not stop; leaking it");
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    gba_core_halt();
    heap_caps_free(s_blank);
    s_blank = NULL;
    s_img = NULL;
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

const os_app_t app_gbaemu = {
    .id = "gbaemu",
    .name = "Game Boy Advance",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
};
