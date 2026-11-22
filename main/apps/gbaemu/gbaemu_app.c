// Game Boy Advance: plays a .gba ROM from the microSD card (/sdcard/roms).
//
// The CPU/PPU core is 44VBA (vba-next fork, GPL-3), vendored under vendor/44vba.
// No ROM ships with the firmware: copy a legally owned dump onto the card.
//
// Threads: OS callbacks run on the OS task (LVGL locked). Emulation runs in
// its own FreeRTOS task at priority 3 (below the prio-4 blit helper and OS),
// with the 16 KB stack in PSRAM so xTaskCreate still works after Game Boy
// leaves internal heap fragmented (~28 KB free).
// While a ROM runs the OS task sleeps on the button queue plus a 5 s battery
// tick, so it does not steal the chip. Matching OS/blit at 4 was tried:
// Advance Wars gained a little, GTA Advance (paged) dropped to ~8 emu fps.
// Background radios and services (Wi-Fi, ESP-NOW mesh, cloud, game tick,
// Lua host, BLE) pause for the ROM and restore on the list, HOME, or exit.
//
// Screen: native 240x160 centred under the 22 px status bar (panel 320x240).
// Drawn frames are packed into a PSRAM double-buffer (2x76 KB, not the old
// 2x153 KB fullscreen stretch) and a prio-4 blit task sends them to the
// ILI9341 (not an LVGL image). Pack buffers are allocated after the ROM so
// a 4 MB cart still gets the large PSRAM block. The interpreter only copies;
// it does not wait on SPI, so DMA runs while the next frames emulate. LVGL's
// refresh timer is paused while a ROM runs. Status-bar objects still tick
// every 5 s but are not flushed (SPI2 is the SD card). Frameskip 2: 1 of 3
// PPU frames is drawn.
//
// Button chords (6 front keys + BOOT on the back; calibrated os_key map):
//   d-pad                 GBA d-pad
//   SELECT                A
//   CANCEL                B
//   UP+DOWN               START (latched; d-pad swallowed until both release)
//   SELECT+CANCEL         START (second mapping; does not steal HOME)
//   LEFT+RIGHT            SELECT
//   LEFT+RIGHT+UP / DOWN  L / R
//   SELECT+START          exit to ROM list  (badge SELECT + UP+DOWN)
//   SELECT+A              write battery .sav now  (GBA SELECT + A:
//                         LEFT+RIGHT + badge SELECT)
//   BOOT (HOME)           OS exit; save RAM is written
//   console `key start|l|r` injects those GBA buttons for 350 ms
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
#ifndef SOLANA_OS_EMULATOR
#include "freertos/idf_additions.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp.h"
#include "core/os.h"
#include "core/ui.h"
#include "apps/emu_pad.h"
#include "gbaemu_host.h"

static const char *TAG = "gbaemu";

#define FRAME_US        16743
#define SAVE_EVERY_US   (60 * 1000000LL)
#define SAVE_IDLE_US    (2 * 1000000LL)
#define MAX_ROMS        32
#define PATH_MAX_LEN    200
#define CHORD_MS        250
#define ROMS_DIR        BSP_SD_MOUNT "/roms"
#define TASK_STACK      16384
#define TASK_PRIO       3   // below blit/OS; OS is parked on the button queue during play
#define BLIT_TASK_STACK 3072
#define BLIT_TASK_PRIO  4   // same as LVGL: start DMA, then block so emu runs
#define GBA_BLIT_X      ((BOARD_LCD_H_RES - GBA_WIDTH) / 2)
#define GBA_BLIT_Y      (OS_STATUS_H + (OS_ROOT_H - GBA_HEIGHT) / 2)
#define GBA_FB_BYTES    ((size_t)GBA_WIDTH * (size_t)GBA_HEIGHT * sizeof(uint16_t))

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static volatile bool s_back_to_list;

// Direct blit: emu packs into the buffer the DMA task is not sending. Pending
// is cleared while packing so the blit task cannot start DMA on a live copy.
static uint16_t *s_blit_fb[2];
static TaskHandle_t s_blit_task;
static SemaphoreHandle_t s_blit_kick;
static SemaphoreHandle_t s_blit_done;
static portMUX_TYPE s_blit_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_blit_pending = -1;
static volatile int s_blit_sending = -1;
static volatile int s_blit_packing = -1;
static volatile bool s_blit_quit;
static int64_t s_exit_since, s_save_since;
static bool s_save_armed = true;
static lv_obj_t *s_root, *s_msg, *s_hint;
static ui_list_t *s_list;
static char s_roms[MAX_ROMS][PATH_MAX_LEN];
static char s_play_path[PATH_MAX_LEN];
static char s_sav_path[PATH_MAX_LEN];
static int s_rom_count;
static emu_pad_t s_pad;

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
    uint32_t held = emu_pad_joy(&s_pad);
    emu_pad_log(&s_pad, held, TAG);
    return held;
}

static void chords(int64_t now)
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
            gba_core_save(s_sav_path, true);
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
            if (i < 0 || i == s_blit_packing) {
                portEXIT_CRITICAL(&s_blit_mux);
                break;
            }
            s_blit_pending = -1;
            s_blit_sending = i;
            portEXIT_CRITICAL(&s_blit_mux);
            (void)bsp_lcd_blit_rgb565(GBA_BLIT_X, GBA_BLIT_Y, GBA_WIDTH, GBA_HEIGHT,
                                      s_blit_fb[i], GBA_WIDTH);
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

static bool present(const uint16_t *fb)
{
    if (s_quit || !fb) {
        return false;
    }
    if (!s_blit_fb[0] || !s_blit_fb[1] || !s_blit_kick) {
        return bsp_lcd_blit_rgb565(GBA_BLIT_X, GBA_BLIT_Y, GBA_WIDTH, GBA_HEIGHT,
                                   fb, GBA_FB_STRIDE) == ESP_OK;
    }

    portENTER_CRITICAL(&s_blit_mux);
    int i = (s_blit_sending == 0) ? 1 : 0;
    if (s_blit_pending == i) {
        s_blit_pending = -1;
    }
    s_blit_packing = i;
    portEXIT_CRITICAL(&s_blit_mux);

    uint16_t *dst = s_blit_fb[i];
    const uint16_t *src = fb;
    for (int r = 0; r < GBA_HEIGHT; r++) {
        memcpy(dst, src, GBA_WIDTH * sizeof(uint16_t));
        dst += GBA_WIDTH;
        src += GBA_FB_STRIDE;
    }

    portENTER_CRITICAL(&s_blit_mux);
    s_blit_packing = -1;
    s_blit_pending = i;
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
    heap_caps_free(s_blit_fb[0]);
    heap_caps_free(s_blit_fb[1]);
    s_blit_fb[0] = s_blit_fb[1] = NULL;
    s_blit_pending = s_blit_sending = s_blit_packing = -1;
}

static bool blit_start(void)
{
    blit_stop();
    s_blit_fb[0] = heap_caps_malloc(GBA_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_blit_fb[1] = heap_caps_malloc(GBA_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_blit_kick = xSemaphoreCreateBinary();
    s_blit_done = xSemaphoreCreateBinary();
    s_blit_quit = false;
    s_blit_pending = s_blit_sending = s_blit_packing = -1;
    if (!s_blit_fb[0] || !s_blit_fb[1] || !s_blit_kick || !s_blit_done ||
        xTaskCreate(blit_task, "gbablit", BLIT_TASK_STACK, NULL,
                    BLIT_TASK_PRIO, &s_blit_task) != pdPASS) {
        ESP_LOGW(TAG, "async blit unavailable (PSRAM %u largest %u); SPI waits on the emu task",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        blit_stop();
        return false;
    }
    ESP_LOGI(TAG, "native blit 240x160 at (%d,%d), pack %u B x2",
             GBA_BLIT_X, GBA_BLIT_Y, (unsigned)GBA_FB_BYTES);
    return true;
}

static void play_video(bool on)
{
    if (on) {
        // Keep the last LVGL frame (a "Loading…" label) on the panel while
        // the ROM copies. Painting black here made 8 MB paged carts look dead
        // for several seconds before the first 240x160 blit.
        bsp_display_pause_lvgl(true);
        // blit_start() waits until gba_core_boot so 4 MB carts still get
        // the large PSRAM block. Pack buffers are native 2x76 KB.
    } else {
        blit_stop();
        bsp_display_pause_lvgl(false);
        if (s_root) {
            lv_obj_set_hidden(s_root, false);
        }
    }
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
    os_background_unpause();
    s_back_to_list = false;
    play_video(false);
    if (s_root) {
        lv_obj_clean(s_root);
        show_picker();
    }
}

static void emu_task(void *arg)
{
    const char *path = (const char *)arg;
    ESP_LOGI(TAG, "emu_task %s", path);
    sav_from_rom(s_sav_path, sizeof(s_sav_path), path);
    os_background_pause();
    if (!gba_core_boot(path, s_sav_path)) {
        s_back_to_list = true;
        goto out;
    }
    if (!blit_start()) {
        ESP_LOGW(TAG, "pack buffers unavailable; blit native 240x160 on the emu task");
    }
    ESP_LOGI(TAG, "running %s", gba_core_title()[0] ? gba_core_title() : path);
    s_exit_since = s_save_since = 0;
    s_save_armed = true;
    emu_pad_reset(&s_pad);

    int64_t next = esp_timer_get_time(), last_save = next, stat_t = next, last_touch = next;
    int64_t last_idle = next;
    int emulated = 0, shown = 0, late = 0;
    uint32_t miss0 = gba_core_rom_misses();
    while (!s_quit) {
        int64_t now = esp_timer_get_time();
        emu_pad_sample(&s_pad);
        chords(now);
        if (s_quit) {
            break;
        }
        const uint16_t *fb = NULL;
        int n = gba_core_frame(joypad(), &fb);
        if (n > 0) {
            emulated += n;
            shown += present(fb);
            if (gba_core_save_dirty()) {
                last_touch = now;
            }
            next += (int64_t)n * FRAME_US;
        }
        now = esp_timer_get_time();
        // IDLE (prio 0) pets the task WDT. Yielding never runs it; one tick
        // every ~1 s is enough and costs ~1% vs 10 ms every frame.
        if (now - last_idle >= 1000000) {
            vTaskDelay(1);
            last_idle = now;
        } else if (next - now >= 10000) {
            vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        } else {
            if (now - next > 100000) {
                next = now;
                late++;
            }
            taskYIELD();
        }
        if (now - stat_t >= 5000000) {
            double s = (now - stat_t) / 1e6;
            uint32_t miss = gba_core_rom_misses();
            ESP_LOGI(TAG, "%.1f emulated fps, %.1f shown (skip 2), %.0f ms/emu-frame, %d resyncs, %u page misses",
                     emulated / s, shown / s, s * 1000.0 / (emulated > 0 ? emulated : 1), late,
                     (unsigned)(miss - miss0));
            miss0 = miss;
            stat_t = now;
            emulated = shown = late = 0;
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
    vTaskDeleteWithCaps(NULL);
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
    message("Loading…");
    lv_refr_now(NULL);
    play_video(true);

    s_quit = false;
    s_back_to_list = false;
    strlcpy(s_play_path, path, sizeof(s_play_path));
    ESP_LOGI(TAG, "start %s", s_play_path);
    // Park Wi-Fi before the emu task exists. After Game Boy, internal heap is
    // ~28 KB free and fragmented; a 16 KB SRAM stack then fails xTaskCreate.
    os_background_pause();
    s_done = xSemaphoreCreateBinary();
    if (!s_done || xTaskCreateWithCaps(emu_task, "gbaemu", TASK_STACK, s_play_path,
                                       TASK_PRIO, &s_task,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate gbaemu failed (done %p SRAM %u largest %u)",
                 s_done, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        os_toast("Can't start the emulator task");
        if (s_done) {
            vSemaphoreDelete(s_done);
            s_done = NULL;
        }
        s_task = NULL;
        os_background_unpause();
        play_video(false);
        lv_obj_clean(s_root);
        show_picker();
        return;
    }
    os_toast("UP+DOWN or SELECT+CANCEL = Start");
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

const os_app_t app_gbaemu = {
    .id = "gbaemu",
    .name = "Game Boy Advance",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
};
