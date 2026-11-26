// Silent MJPEG player: list .mjpeg/.mjpg (and JPEG-sequence folders) under
// /sdcard/videos, decode baseline JPEG frames with TJpgDec, blit to the
// ILI9341 via bsp_lcd_blit_rgb565. No audio. Target ~8–12 fps at 320×240.

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"
#include "vidplayer_jpeg.h"

static const char *TAG = "vidplayer";

#define VIDEOS_DIR      BSP_SD_MOUNT "/videos"
#define MAX_CLIPS       48
#define PATH_MAX_LEN  256
#define FRAME_MAX       (48 * 1024)     // one JPEG frame
#define PANEL_W         BOARD_LCD_H_RES
#define PANEL_H         BOARD_LCD_V_RES
#define TARGET_FPS      10
#define FRAME_US        (1000000 / TARGET_FPS)
#define TASK_PRIO       3
#define TASK_STACK      (12 * 1024)

typedef enum { CLIP_MJPEG = 0, CLIP_SEQ } clip_kind_t;

typedef struct {
    char path[PATH_MAX_LEN];
    clip_kind_t kind;
} clip_t;

static lv_obj_t *s_root, *s_hint, *s_msg;
static ui_list_t *s_list;
static clip_t s_clips[MAX_CLIPS];
static int s_nclips;

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static volatile bool s_paused;
static volatile bool s_back_to_list;
static char s_play_path[PATH_MAX_LEN];
static clip_kind_t s_play_kind;

static uint16_t *s_fb;
static uint8_t *s_jpg;

static void show_picker(void);
static void play_video(bool on);

// ---- helpers --------------------------------------------------------------

static bool ends_with(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && !strcasecmp(name + n - e, ext);
}

static bool is_mjpeg(const char *name)
{
    return ends_with(name, ".mjpeg") || ends_with(name, ".mjpg");
}

static bool is_jpeg(const char *name)
{
    return ends_with(name, ".jpg") || ends_with(name, ".jpeg");
}

static int cmp_str(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static bool dir_has_jpegs(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    bool ok = false;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (is_jpeg(de->d_name)) {
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

static void add_clip(const char *path, clip_kind_t kind)
{
    if (s_nclips >= MAX_CLIPS || strlen(path) >= PATH_MAX_LEN) {
        return;
    }
    strlcpy(s_clips[s_nclips].path, path, PATH_MAX_LEN);
    s_clips[s_nclips].kind = kind;
    s_nclips++;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && s_nclips < MAX_CLIPS) {
        if (de->d_name[0] == '.') {
            continue;
        }
        size_t dl = strlen(dir), nl = strlen(de->d_name);
        if (dl + 1 + nl >= PATH_MAX_LEN) {
            continue;
        }
        char path[PATH_MAX_LEN];
        memcpy(path, dir, dl);
        path[dl] = '/';
        memcpy(path + dl + 1, de->d_name, nl + 1);

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISREG(st.st_mode) && is_mjpeg(de->d_name)) {
            add_clip(path, CLIP_MJPEG);
        } else if (S_ISDIR(st.st_mode) && dir_has_jpegs(path)) {
            add_clip(path, CLIP_SEQ);
        }
    }
    closedir(d);
}

static void clip_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    const char *slash = strrchr(s_clips[i].path, '/');
    const char *name = slash ? slash + 1 : s_clips[i].path;
    strlcpy(row->title, name, sizeof(row->title));
    if (s_clips[i].kind == CLIP_SEQ) {
        strlcpy(row->sub, "JPEG sequence folder", sizeof(row->sub));
        strlcpy(row->right, "seq", sizeof(row->right));
    } else {
        strlcpy(row->sub, "MJPEG (silent)", sizeof(row->sub));
        strlcpy(row->right, "mjpeg", sizeof(row->right));
    }
    row->right_color = COL_DIM;
}

static void message(const char *text)
{
    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    s_msg = ui_label(s_root, &sol_font_14, COL_TEXT, text);
    lv_obj_set_width(s_msg, OS_ROOT_W - 32);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, -8);
}

static void clear_picker_ui(void)
{
    if (s_list) {
        ui_list_show(s_list, false);
        s_list = NULL;
    }
    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    if (s_hint) {
        lv_obj_delete(s_hint);
        s_hint = NULL;
    }
}

static void play_video(bool on)
{
    if (on) {
        bsp_display_pause_lvgl(true);
    } else {
        bsp_display_pause_lvgl(false);
        if (s_root) {
            lv_obj_set_hidden(s_root, false);
        }
    }
}

static int choose_scale(int w, int h)
{
    for (int s = 0; s <= 3; s++) {
        int div = 1 << s;
        if (w / div <= PANEL_W && h / div <= PANEL_H) {
            return s;
        }
    }
    return 3;
}

// Read next JPEG from an MJPEG FILE* into s_jpg; returns length or 0 at EOF/err.
static size_t read_mjpeg_frame(FILE *f)
{
    // Find SOI (FF D8)
    int b0 = -1;
    for (;;) {
        int b = fgetc(f);
        if (b == EOF) {
            return 0;
        }
        if (b0 == 0xFF && b == 0xD8) {
            break;
        }
        b0 = b;
    }
    size_t n = 0;
    s_jpg[n++] = 0xFF;
    s_jpg[n++] = 0xD8;
    int prev = 0xD8;
    while (n < FRAME_MAX) {
        int b = fgetc(f);
        if (b == EOF) {
            return 0;
        }
        s_jpg[n++] = (uint8_t)b;
        if (prev == 0xFF && b == 0xD9) {
            return n;
        }
        prev = b;
    }
    ESP_LOGW(TAG, "frame > %d bytes, skip", FRAME_MAX);
    // Resync: keep scanning for EOI then next SOI on next call
    return 0;
}

static int list_seq_jpgs(const char *dir, char ***out_names, int *out_n)
{
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!is_jpeg(de->d_name)) {
            continue;
        }
        if (n >= cap) {
            int ncap = cap ? cap * 2 : 16;
            char **nn = realloc(names, (size_t)ncap * sizeof(char *));
            if (!nn) {
                break;
            }
            names = nn;
            cap = ncap;
        }
        names[n] = strdup(de->d_name);
        if (!names[n]) {
            break;
        }
        n++;
    }
    closedir(d);
    if (n > 1) {
        qsort(names, (size_t)n, sizeof(char *), cmp_str);
    }
    *out_names = names;
    *out_n = n;
    return 0;
}

static void free_seq(char **names, int n)
{
    if (!names) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
}

static bool decode_blit(const uint8_t *jpg, size_t len)
{
    // Peek SOF dimensions without a full decode: try scale 0 info; on failure bail.
    vidplayer_jpeg_info_t info;
    if (!vidplayer_jpeg_info(jpg, len, 0, &info)) {
        return false;
    }
    int scale = choose_scale((int)info.width, (int)info.height);
    // Re-decode at chosen scale (second jd_prepare — frames are small).
    if (!vidplayer_jpeg_decode(jpg, len, scale, s_fb, PANEL_W, PANEL_H, PANEL_W, true, &info)) {
        return false;
    }
    return bsp_lcd_blit_rgb565(0, 0, PANEL_W, PANEL_H, s_fb, PANEL_W) == ESP_OK;
}

static void back_to_list(void *arg)
{
    (void)arg;
    s_task = NULL;
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    play_video(false);
    if (s_root) {
        lv_obj_clean(s_root);
        show_picker();
    }
}

static void play_task(void *arg)
{
    (void)arg;
    int64_t next = esp_timer_get_time();
    int shown = 0, late = 0;
    int64_t stat_t = next;

    if (s_play_kind == CLIP_MJPEG) {
        FILE *f = fopen(s_play_path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "open %s", s_play_path);
            goto out;
        }
        while (!s_quit) {
            while (s_paused && !s_quit) {
                vTaskDelay(pdMS_TO_TICKS(50));
                next = esp_timer_get_time();
            }
            if (s_quit) {
                break;
            }
            size_t n = read_mjpeg_frame(f);
            if (!n) {
                // Loop from start
                clearerr(f);
                fseek(f, 0, SEEK_SET);
                n = read_mjpeg_frame(f);
                if (!n) {
                    break;
                }
            }
            if (decode_blit(s_jpg, n)) {
                shown++;
            }
            int64_t now = esp_timer_get_time();
            next += FRAME_US;
            if (next - now >= 5000) {
                vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
            } else if (now - next > 100000) {
                next = now;
                late++;
            } else {
                taskYIELD();
            }
            now = esp_timer_get_time();
            if (now - stat_t >= 5000000) {
                double s = (now - stat_t) / 1e6;
                ESP_LOGI(TAG, "%.1f fps shown, %d late", shown / s, late);
                shown = late = 0;
                stat_t = now;
            }
        }
        fclose(f);
    } else {
        char **names = NULL;
        int nnames = 0;
        if (list_seq_jpgs(s_play_path, &names, &nnames) != 0 || nnames == 0) {
            ESP_LOGE(TAG, "no jpegs in %s", s_play_path);
            free_seq(names, nnames);
            goto out;
        }
        int i = 0;
        while (!s_quit) {
            while (s_paused && !s_quit) {
                vTaskDelay(pdMS_TO_TICKS(50));
                next = esp_timer_get_time();
            }
            if (s_quit) {
                break;
            }
            char path[512];
            snprintf(path, sizeof(path), "%.200s/%.200s", s_play_path, names[i]);
            FILE *f = fopen(path, "rb");
            size_t n = 0;
            if (f) {
                n = fread(s_jpg, 1, FRAME_MAX, f);
                fclose(f);
            }
            if (n >= 4 && decode_blit(s_jpg, n)) {
                shown++;
            }
            i++;
            if (i >= nnames) {
                i = 0;
            }
            int64_t now = esp_timer_get_time();
            next += FRAME_US;
            if (next - now >= 5000) {
                vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
            } else if (now - next > 100000) {
                next = now;
                late++;
            } else {
                taskYIELD();
            }
        }
        free_seq(names, nnames);
    }

out:
    if (s_done) {
        xSemaphoreGive(s_done);
    }
    if (s_back_to_list) {
        os_post(back_to_list, NULL);
    }
    vTaskDeleteWithCaps(NULL);
}

static bool alloc_bufs(void)
{
    if (!s_fb) {
        s_fb = heap_caps_malloc((size_t)PANEL_W * PANEL_H * sizeof(uint16_t), os_mem_bulk_caps());
    }
    if (!s_jpg) {
        s_jpg = heap_caps_malloc(FRAME_MAX, os_mem_bulk_caps());
    }
    return s_fb && s_jpg;
}

static void free_bufs(void)
{
    free(s_fb);
    s_fb = NULL;
    free(s_jpg);
    s_jpg = NULL;
}

static void start_clip(int idx)
{
#ifdef SOLANA_OS_EMULATOR
    (void)idx;
    os_toast("Video plays on the badge, not the web emulator");
    return;
#else
    if (idx < 0 || idx >= s_nclips) {
        return;
    }
    if (!alloc_bufs()) {
        os_toast("Out of memory");
        free_bufs();
        return;
    }
    clear_picker_ui();
    message("Loading…");
    lv_refr_now(NULL);

    strlcpy(s_play_path, s_clips[idx].path, sizeof(s_play_path));
    s_play_kind = s_clips[idx].kind;
    s_quit = false;
    s_paused = false;
    s_back_to_list = false;
    play_video(true);
    if (s_root) {
        lv_obj_set_hidden(s_root, true);
    }

    s_done = xSemaphoreCreateBinary();
    if (!s_done ||
        xTaskCreateWithCaps(play_task, "vidplayer", TASK_STACK, NULL, TASK_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        os_toast("Can't start player");
        if (s_done) {
            vSemaphoreDelete(s_done);
            s_done = NULL;
        }
        s_task = NULL;
        play_video(false);
        if (s_root) {
            lv_obj_clean(s_root);
            show_picker();
        }
        return;
    }
    os_toast("SELECT pause   CANCEL list");
#endif
}

// From on_exit / HOME: wait for the worker. From CANCEL while playing: just
// signal — the worker posts back_to_list when it finishes (don't block OS).
static void stop_play(bool wait)
{
    if (!s_task) {
        return;
    }
    s_quit = true;
    s_paused = false;
    if (!wait) {
        s_back_to_list = true;
        return;
    }
    s_back_to_list = false;
    if (s_done && xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "play task did not stop");
    }
    s_task = NULL;
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    play_video(false);
}

static void show_picker(void)
{
    s_list = NULL;
    s_msg = s_hint = NULL;
    s_nclips = 0;

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nInsert one with clips in /videos.");
        return;
    }
    scan_dir(VIDEOS_DIR);
    ESP_LOGI(TAG, "picker %d clips in %s", s_nclips, VIDEOS_DIR);
    if (!s_nclips) {
        message("No clips found.\nPut .mjpeg/.mjpg (or a folder of .jpg) in /videos.");
        return;
    }
    s_list = ui_list_create(s_root, 0, OS_ROOT_H - UI_HINT_H, clip_row, NULL);
    ui_list_set_count(s_list, s_nclips);
    s_hint = ui_hint(s_root, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT play   CANCEL exit");
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_task = NULL;
    s_done = NULL;
    s_quit = false;
    s_paused = false;
    s_back_to_list = false;
    show_picker();
}

static void leave(void)
{
    s_back_to_list = false;
    stop_play(true);
    free_bufs();
    s_root = NULL;
}

static bool on_key(os_key_t key, bool repeat)
{
    if (s_task) {
        if (key == OS_KEY_SELECT && !repeat) {
            s_paused = !s_paused;
            os_toast(s_paused ? "Paused" : "Playing");
            return true;
        }
        if (key == OS_KEY_BACK && !repeat) {
            stop_play(false);   // worker returns to picker
            return true;
        }
        return true;    // swallow while playing (HOME still exits via OS)
    }
    if (s_list && key == OS_KEY_SELECT && !repeat) {
        int i = ui_list_selected(s_list);
        if (i >= 0) {
            start_clip(i);
        }
        return true;
    }
    if (s_list && ui_list_key(s_list, key, repeat)) {
        return true;
    }
    return false;       // CANCEL on picker → exit app
}

const os_app_t app_vidplayer = {
    .id = "vidplayer",
    .name = "Video",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
};
