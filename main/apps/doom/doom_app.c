// Doom: plays an IWAD (.wad) from the microSD card (/sdcard/doom).
//
// Core is doomgeneric (Chocolate Doom) with ESP-DOOM shutdown path, vendored
// under vendor/doomgeneric. Sound is muted (-nosound). No WAD ships with the
// firmware: copy a legally owned IWAD onto the card (shareware doom1.wad is
// freely redistributable).
//
// Threads: OS callbacks on the OS task (LVGL locked). Game loop in its own
// FreeRTOS task at priority 3 (below prio-4 blit/OS), 32 KB stack in PSRAM.
// While a WAD runs, Wi-Fi / mesh / cloud / game tick / Lua / BLE pause.
//
// Screen: native 320x200 letterboxed on 320x240 (20px bars). PSRAM
// double-buffer + prio-4 blit task. Low-detail render + palette LUT.
//
// Button chords (6 front keys + BOOT):
//   d-pad                 move
//   SELECT                fire
//   CANCEL                use
//   UP+DOWN / SELECT+CANCEL  Enter (menu / confirm)
//   LEFT+RIGHT            Escape (menu)
//   SELECT+UP+DOWN        exit to WAD list
//   BOOT (HOME)           OS exit

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
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "bsp.h"
#include "core/os.h"
#include "core/theme_battle.h"
#include "core/ui.h"
#include "apps/emu_pad.h"
#include "doom_host.h"

static const char *TAG = "doom";

#define MAX_WADS        32
#define PATH_MAX_LEN    200
#define CHORD_MS        250
#define WADS_DIR        BSP_SD_MOUNT "/doom"
#define TASK_STACK      32768
#define TASK_PRIO       4
#define BLIT_TASK_STACK 3072
#define BLIT_TASK_PRIO  4
#define SCALE_W         DOOM_WIDTH
#define SCALE_H         DOOM_HEIGHT
#define DOOM_BLIT_X     0
#define DOOM_BLIT_Y     ((BOARD_LCD_V_RES - DOOM_HEIGHT) / 2)
#define DOOM_FB_BYTES   ((size_t)SCALE_W * (size_t)SCALE_H * sizeof(uint16_t))
#define FRAME_US        50000   /* target ~20 Hz — matches C5 budget better than 35 */
#define STAT_SAMPLE_STEP 8      /* sparse nonblack (every 8th pixel) */

_Static_assert(DOOM_WIDTH == BOARD_LCD_H_RES, "Doom width matches panel");
_Static_assert(DOOM_HEIGHT + 2 * DOOM_BLIT_Y == BOARD_LCD_V_RES, "letterbox 200 in 240");

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static volatile bool s_back_to_list;

static uint16_t *s_blit_fb[2];
static TaskHandle_t s_blit_task;
static SemaphoreHandle_t s_blit_kick;
static SemaphoreHandle_t s_blit_done;
static portMUX_TYPE s_blit_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_blit_pending = -1;
static volatile int s_blit_sending = -1;
static volatile int s_blit_packing = -1;
static volatile int s_blit_shot = -1;
static volatile bool s_blit_quit;
static volatile unsigned s_fb_nonblack;
static volatile bool s_have_picture;
static int64_t s_exit_since;
static lv_obj_t *s_root, *s_msg, *s_hint, *s_banner;
static ui_list_t *s_list;
static EXT_RAM_BSS_ATTR char s_wads[MAX_WADS][PATH_MAX_LEN];
static char s_play_path[PATH_MAX_LEN];
static int s_wad_count;
static emu_pad_t s_pad;

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
            (void)bsp_lcd_blit_rgb565(DOOM_BLIT_X, DOOM_BLIT_Y, SCALE_W, SCALE_H,
                                      s_blit_fb[i], SCALE_W);
            portENTER_CRITICAL(&s_blit_mux);
            s_blit_sending = -1;
            portEXIT_CRITICAL(&s_blit_mux);
        }
    }
    if (s_blit_done) {
        xSemaphoreGive(s_blit_done);
    }
#ifndef SOLANA_OS_EMULATOR
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

static unsigned fb_nonblack_sparse(const uint16_t *fb, int stride)
{
    unsigned n = 0;
    for (int r = 0; r < DOOM_HEIGHT; r += STAT_SAMPLE_STEP) {
        const uint16_t *row = fb + r * stride;
        for (int c = 0; c < DOOM_WIDTH; c += STAT_SAMPLE_STEP) {
            if (row[c]) {
                n++;
            }
        }
    }
    /* Scale sample count toward full-frame estimate for logs. */
    return n * (STAT_SAMPLE_STEP * STAT_SAMPLE_STEP);
}

static bool present(const uint16_t *fb)
{
    if (s_quit || !fb) {
        return false;
    }
    if (!s_have_picture) {
        unsigned nb = fb_nonblack_sparse(fb, DOOM_FB_STRIDE);
        s_fb_nonblack = nb;
        if (nb < 80) {
            return true;
        }
        s_have_picture = true;
    }

    /* Frameskip: if both blit buffers are busy, drop this frame's blit. */
    if (s_blit_fb[0] && s_blit_fb[1] && s_blit_kick) {
        portENTER_CRITICAL(&s_blit_mux);
        bool busy0 = (s_blit_sending == 0 || s_blit_shot == 0 || s_blit_pending == 0);
        bool busy1 = (s_blit_sending == 1 || s_blit_shot == 1 || s_blit_pending == 1);
        if (busy0 && busy1) {
            portEXIT_CRITICAL(&s_blit_mux);
            return false; /* skipped */
        }
        int i = busy0 ? 1 : 0;
        if (s_blit_pending == i) {
            s_blit_pending = -1;
        }
        s_blit_packing = i;
        portEXIT_CRITICAL(&s_blit_mux);

        memcpy(s_blit_fb[i], fb, DOOM_FB_BYTES);

        portENTER_CRITICAL(&s_blit_mux);
        s_blit_packing = -1;
        s_blit_pending = i;
        portEXIT_CRITICAL(&s_blit_mux);
        xSemaphoreGive(s_blit_kick);
        return true;
    }

    return bsp_lcd_blit_rgb565(DOOM_BLIT_X, DOOM_BLIT_Y, SCALE_W, SCALE_H,
                               fb, DOOM_FB_STRIDE) == ESP_OK;
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
    s_blit_pending = s_blit_sending = s_blit_packing = s_blit_shot = -1;
    s_have_picture = false;
    s_fb_nonblack = 0;
}

static bool doom_direct_shot(void)
{
    uint16_t *fb = NULL;
    unsigned nb = s_fb_nonblack;
    bool picture = s_have_picture;

    portENTER_CRITICAL(&s_blit_mux);
    int i = s_blit_pending;
    if (i < 0 || i == s_blit_packing) {
        i = s_blit_sending;
    }
    if (i < 0 || i == s_blit_packing) {
        i = (s_blit_packing == 0) ? 1 : 0;
    }
    if (s_blit_fb[i]) {
        s_blit_shot = i;
        fb = s_blit_fb[i];
    }
    portEXIT_CRITICAL(&s_blit_mux);

    esp_log_level_set("*", ESP_LOG_NONE);
    printf("\ndoom shot %s %u nonblack\n", picture ? "picture" : "loading", (unsigned)nb);
    printf("SHOT-BEGIN %d %d\n", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    static uint16_t row[BOARD_LCD_H_RES];
    static unsigned char b64[BOARD_LCD_H_RES * 2 * 4 / 3 + 8];
    for (int y = 0; y < BOARD_LCD_V_RES; y++) {
        memset(row, 0, sizeof(row));
        int sy = y - DOOM_BLIT_Y;
        if (picture && fb && sy >= 0 && sy < SCALE_H) {
            memcpy(row, fb + sy * SCALE_W, SCALE_W * sizeof(uint16_t));
        }
        size_t n = 0;
        mbedtls_base64_encode(b64, sizeof(b64), &n, (const unsigned char *)row, sizeof(row));
        b64[n] = 0;
        printf("SHOT %d %d %d %s\n", 0, y, BOARD_LCD_H_RES, b64);
    }
    printf("SHOT-END\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);

    portENTER_CRITICAL(&s_blit_mux);
    s_blit_shot = -1;
    portEXIT_CRITICAL(&s_blit_mux);
    return true;
}

static bool blit_start(void)
{
    blit_stop();
    s_blit_fb[0] = heap_caps_malloc(DOOM_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_blit_fb[1] = heap_caps_malloc(DOOM_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_blit_kick = xSemaphoreCreateBinary();
    s_blit_done = xSemaphoreCreateBinary();
    s_blit_quit = false;
    s_blit_pending = s_blit_sending = s_blit_packing = s_blit_shot = -1;
    bool task_ok = false;
    if (s_blit_fb[0] && s_blit_fb[1] && s_blit_kick && s_blit_done) {
#ifndef SOLANA_OS_EMULATOR
        task_ok = xTaskCreateWithCaps(blit_task, "doomblit", BLIT_TASK_STACK, NULL,
                                      BLIT_TASK_PRIO, &s_blit_task,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
#else
        task_ok = xTaskCreate(blit_task, "doomblit", BLIT_TASK_STACK, NULL,
                              BLIT_TASK_PRIO, &s_blit_task) == pdPASS;
#endif
    }
    if (!task_ok) {
        ESP_LOGW(TAG, "async blit unavailable");
        if (s_blit_kick) {
            vSemaphoreDelete(s_blit_kick);
            s_blit_kick = NULL;
        }
        if (s_blit_done) {
            vSemaphoreDelete(s_blit_done);
            s_blit_done = NULL;
        }
        s_blit_task = NULL;
        heap_caps_free(s_blit_fb[1]);
        s_blit_fb[1] = NULL;
        if (!s_blit_fb[0]) {
            blit_stop();
        }
        return false;
    }
    ESP_LOGI(TAG, "letterbox blit 320x200 @ y=%d", DOOM_BLIT_Y);
    return true;
}

static void play_video(bool on)
{
    if (on) {
        os_set_direct_shot(doom_direct_shot);
        bsp_display_pause_lvgl(true);
    } else {
        os_set_direct_shot(NULL);
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
            ESP_LOGW(TAG, "doom task did not stop; leaking it");
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    doom_core_halt();
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
    os_background_pause();
    if (!doom_core_boot(path)) {
        ESP_LOGE(TAG, "boot failed %s", path);
        os_toast("Doom failed to start");
        s_back_to_list = true;
        goto out;
    }
    (void)blit_start();
    ESP_LOGI(TAG, "running %s", doom_core_title()[0] ? doom_core_title() : path);
    s_exit_since = 0;
    emu_pad_reset(&s_pad);

    int64_t next = esp_timer_get_time(), stat_t = next, last_idle = next;
    int emulated = 0, shown = 0, skipped = 0, late = 0;
    s_have_picture = false;
    s_fb_nonblack = 0;
    while (!s_quit) {
        int64_t now = esp_timer_get_time();
        emu_pad_sample(&s_pad);
        chords(now);
        if (s_quit) {
            break;
        }
        const uint16_t *fb = NULL;
        int n = doom_core_tick(joypad(), &fb);
        if (n < 0) {
            s_back_to_list = true;
            break;
        }
        if (n > 0) {
            emulated += n;
            if (present(fb)) {
                shown++;
            } else if (fb) {
                skipped++;
            }
            next += FRAME_US;
        }
        now = esp_timer_get_time();
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
            if (fb) {
                s_fb_nonblack = fb_nonblack_sparse(fb, DOOM_FB_STRIDE);
            }
            ESP_LOGI(TAG, "%.1f ticks/s, %.1f shown, %d skip, %d resyncs, %u nonblack%s",
                     emulated / s, shown / s, skipped, late, (unsigned)s_fb_nonblack,
                     s_have_picture ? "" : " loading");
            stat_t = now;
            emulated = shown = skipped = late = 0;
        }
    }
out:
    blit_stop();
    doom_core_halt();
    xSemaphoreGive(s_done);
    if (s_back_to_list) {
        os_post(back_to_list, NULL);
    }
    vTaskDeleteWithCaps(NULL);
}

static bool is_wad(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".wad");
}

static void scan(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && s_wad_count < MAX_WADS) {
        size_t dl = strlen(dir), nl = strlen(de->d_name);
        if (is_wad(de->d_name) && dl + 1 + nl < PATH_MAX_LEN) {
            char *p = s_wads[s_wad_count++];
            memcpy(p, dir, dl);
            p[dl] = '/';
            memcpy(p + dl + 1, de->d_name, nl + 1);
            ESP_LOGI(TAG, "wad %d %s", s_wad_count - 1, de->d_name);
        }
    }
    closedir(d);
}

static void wad_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    const char *slash = strrchr(s_wads[i], '/');
    strlcpy(row->title, slash ? slash + 1 : s_wads[i], sizeof(row->title));
    strlcpy(row->sub, s_wads[i], sizeof(row->sub));
}

static void paint_banner(void)
{
    s_banner = battle_card(s_root, 8, 4, OS_ROOT_W - 16, 36, T_RED);
    lv_obj_t *title = battle_label(s_banner, &sol_font_20, T_NAVY, "Doom");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, BATTLE_STRIPE_H / 2);
}

static void message(const char *text)
{
    lv_obj_t *card = battle_card(s_root, 16, 56, OS_ROOT_W - 32, 96, T_YELLOW);
    s_msg = battle_label(card, &sol_font_14, T_INK, text);
    lv_obj_set_width(s_msg, OS_ROOT_W - 56);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, BATTLE_STRIPE_H / 2);
}

static void show_picker(void)
{
    s_list = NULL;
    s_msg = s_hint = s_banner = NULL;
    s_wad_count = 0;

    battle_paint_stage(s_root);
    paint_banner();

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nInsert one with a .wad in /doom.");
        s_hint = battle_hint(s_root, "CANCEL exit");
        return;
    }
    scan(WADS_DIR);
    ESP_LOGI(TAG, "picker %d .wad in %s", s_wad_count, WADS_DIR);
    if (!s_wad_count) {
        message("No WAD found.\nPut doom1.wad in /doom\n(tools/sd_put.py).");
        s_hint = battle_hint(s_root, "CANCEL exit");
        return;
    }
    /* List sits under the cream banner; shared ui_list keeps Solana row chrome. */
    s_list = ui_list_create(s_root, 44, OS_ROOT_H - UI_HINT_H - 44, wad_row, NULL);
    ui_list_set_count(s_list, s_wad_count);
    s_hint = battle_hint(s_root, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT play   CANCEL exit");
}

static void start(const char *path)
{
#ifdef SOLANA_OS_EMULATOR
    os_toast("Doom runs on the badge, not the web emulator");
    return;
#else
    if (s_list) {
        ui_list_show(s_list, false);
        s_list = NULL;
    }
    if (s_hint) {
        lv_obj_t *bar = lv_obj_get_parent(s_hint);
        lv_obj_delete(bar);
        s_hint = NULL;
    }
    if (s_msg) {
        lv_obj_t *card = lv_obj_get_parent(s_msg);
        lv_obj_delete(card);
        s_msg = NULL;
    }
    if (s_banner) {
        lv_obj_delete(s_banner);
        s_banner = NULL;
    }
    message("Loading…");
    lv_refr_now(NULL);
    play_video(true);

    s_quit = false;
    s_back_to_list = false;
    strlcpy(s_play_path, path, sizeof(s_play_path));
    ESP_LOGI(TAG, "start %s", s_play_path);
    os_background_pause();
    s_done = xSemaphoreCreateBinary();
    if (!s_done || xTaskCreateWithCaps(emu_task, "doom", TASK_STACK, s_play_path,
                                       TASK_PRIO, &s_task,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate doom failed");
        os_toast("Can't start Doom");
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
    os_toast("SELECT fire/confirm   CANCEL use   L+R Esc");
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
            ESP_LOGW(TAG, "doom task did not stop; leaking it");
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    doom_core_halt();
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
            start(s_wads[i]);
        }
        return true;
    }
    return s_list && ui_list_key(s_list, k, repeat);
}

const os_app_t app_doom = {
    .id = "doom",
    .name = "Doom",
    .symbol = LV_SYMBOL_PLAY,
    .accent = T_RED,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
};
