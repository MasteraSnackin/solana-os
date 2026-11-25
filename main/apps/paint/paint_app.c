// Doodle pad: D-pad moves a cursor, SELECT paints, CANCEL erases / undo,
// LEFT+RIGHT pick a colour, UP+DOWN (START on the six-key pad) clears,
// BOOT (HOME) exits and writes RGB565 to /sdcard/paint/.
//
// LCD and SD share SPI2. A whole-file fwrite lets FATFS dump a cluster as
// 16-sector DMA (8 KB) and allocate_dma_buf fails — LVGL already holds both
// 7.5 KB flush buffers. One 512-byte sector + fsync, with LVGL paused so its
// DMA sits idle, stays inside the pool.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"

static const char *TAG = "paint";

#define CANVAS_W        OS_ROOT_W
#define CANVAS_H        (OS_ROOT_H - UI_HINT_H)
#define BRUSH           4
#define UNDO_MAX        256
#define SECTOR          512
#define PAINT_DIR       BSP_SD_MOUNT "/paint"

static const uint32_t k_colors[] = {
    COL_TEXT, COL_PURPLE, COL_GREEN, COL_PINK, COL_AQUA, COL_YELLOW, COL_ORANGE, COL_BLUE,
};
#define NCOLORS ((int)(sizeof(k_colors) / sizeof(k_colors[0])))

typedef struct {
    uint16_t x, y, px;
} undo_t;

static lv_obj_t *s_canvas, *s_cursor, *s_swatch, *s_hint;
static lv_image_dsc_t s_dsc;
static uint16_t *s_fb;
static int s_x, s_y, s_ci, s_nundo;
static undo_t *s_undo;
static bool s_cleared, s_dirty;

static uint16_t color_u16(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

static uint16_t *px(int x, int y)
{
    return &s_fb[y * CANVAS_W + x];
}

static void set_px(int x, int y, uint16_t v, bool record)
{
    if (x < 0 || y < 0 || x >= CANVAS_W || y >= CANVAS_H) {
        return;
    }
    uint16_t *p = px(x, y);
    if (record && s_undo && s_nundo < UNDO_MAX) {
        s_undo[s_nundo++] = (undo_t){ .x = (uint16_t)x, .y = (uint16_t)y, .px = *p };
    }
    *p = v;
}

static void stamp(uint16_t v, bool record)
{
    for (int dy = 0; dy < BRUSH; dy++) {
        for (int dx = 0; dx < BRUSH; dx++) {
            set_px(s_x + dx, s_y + dy, v, record);
        }
    }
    lv_obj_invalidate(s_canvas);
}

static void place_cursor(void)
{
    lv_obj_set_pos(s_cursor, s_x - 1, s_y - 1);
    lv_obj_set_style_border_color(s_cursor, lv_color_hex(k_colors[s_ci]), 0);
    lv_obj_set_style_bg_color(s_swatch, lv_color_hex(k_colors[s_ci]), 0);
}

static void paint(void)
{
    stamp(color_u16(k_colors[s_ci]), true);
    s_dirty = true;
}

static void erase(void)
{
    stamp(color_u16(COL_BG), true);
    s_dirty = true;
}

static void undo(void)
{
    if (!s_undo || s_nundo <= 0) {
        erase();
        return;
    }
    int n = BRUSH * BRUSH;
    if (n > s_nundo) {
        n = s_nundo;
    }
    for (int i = 0; i < n; i++) {
        undo_t u = s_undo[--s_nundo];
        *px(u.x, u.y) = u.px;
    }
    s_dirty = true;
    lv_obj_invalidate(s_canvas);
}

static void fill_bg(void)
{
    uint16_t bg = color_u16(COL_BG);
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
        s_fb[i] = bg;
    }
    s_nundo = 0;
    if (s_canvas) {
        lv_obj_invalidate(s_canvas);
    }
}

static void clear_canvas(void)
{
    fill_bg();
    s_dirty = true;
    os_toast("cleared");
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            return false;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return fsync(fd) == 0;
}

// PNT1 header + native-endian RGB565, one SD sector per write so DMA stays 512 B.
static bool write_rgb565(const char *path, const uint16_t *fb)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return false;
    }
    uint8_t *blk = heap_caps_malloc(SECTOR, OS_MEM_PSRAM);
    if (!blk) {
        close(fd);
        return false;
    }
    memset(blk, 0, SECTOR);
    memcpy(blk, "PNT1", 4);
    blk[4] = (uint8_t)CANVAS_W;
    blk[5] = (uint8_t)(CANVAS_W >> 8);
    blk[6] = (uint8_t)CANVAS_H;
    blk[7] = (uint8_t)(CANVAS_H >> 8);
    const uint8_t *src = (const uint8_t *)fb;
    size_t pix = (size_t)CANVAS_W * CANVAS_H * 2;
    size_t first = pix < (SECTOR - 8) ? pix : (SECTOR - 8);
    memcpy(blk + 8, src, first);
    bool ok = write_all(fd, blk, 8 + first);
    size_t off = first;
    int nsec = 1;
    while (ok && off < pix) {
        size_t n = pix - off;
        if (n > SECTOR) {
            n = SECTOR;
        }
        memcpy(blk, src + off, n);
        if (n < SECTOR) {
            memset(blk + n, 0, SECTOR - n);
        }
        ok = write_all(fd, blk, n);
        off += n;
        if ((++nsec & 7) == 0) {
            vTaskDelay(1);
        }
    }
    free(blk);
    close(fd);
    return ok;
}

static int next_index(void)
{
    int max = 0;
    DIR *d = opendir(PAINT_DIR);
    if (!d) {
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if ((sscanf(e->d_name, "doodle-%d.rgb565", &n) == 1 ||
             sscanf(e->d_name, "doodle-%d.png", &n) == 1) && n > max) {
            max = n;
        }
    }
    closedir(d);
    return max + 1;
}

static const char *save_doodle(const uint16_t *fb)
{
    bsp_sd_info_t sd;
    bsp_sd_get_info(&sd);
    if (sd.state != BSP_SD_MOUNTED) {
        return "no SD card";
    }
    if (mkdir(PAINT_DIR, 0775) != 0 && errno != EEXIST) {
        return "can't make /sdcard/paint";
    }
    char path[64];
    snprintf(path, sizeof(path), PAINT_DIR "/doodle-%04d.rgb565", next_index());
    if (write_rgb565(path, fb)) {
        ESP_LOGI(TAG, "wrote %s", path);
        return "saved doodle";
    }
    ESP_LOGW(TAG, "save %s failed (DMA %u largest %u)", path,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return "save failed";
}

static void enter(lv_obj_t *root)
{
    s_fb = heap_caps_malloc((size_t)CANVAS_W * CANVAS_H * 2, OS_MEM_PSRAM);
    if (!s_fb) {
        s_fb = malloc((size_t)CANVAS_W * CANVAS_H * 2);
    }
    s_undo = heap_caps_malloc(UNDO_MAX * sizeof(*s_undo), OS_MEM_PSRAM);
    if (!s_undo) {
        s_undo = malloc(UNDO_MAX * sizeof(*s_undo));
    }
    if (!s_fb || !s_undo) {
        ui_label(root, &sol_font_14, COL_ERR, "Not enough memory");
        ui_hint(root, "BOOT exit");
        free(s_fb);
        free(s_undo);
        s_fb = NULL;
        s_undo = NULL;
        return;
    }
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = CANVAS_W;
    s_dsc.header.h = CANVAS_H;
    s_dsc.header.stride = CANVAS_W * 2;
    s_dsc.data_size = (uint32_t)CANVAS_W * CANVAS_H * 2;
    s_dsc.data = (const uint8_t *)s_fb;
    s_canvas = lv_image_create(root);
    lv_image_set_src(s_canvas, &s_dsc);
    lv_obj_set_pos(s_canvas, 0, 0);
    s_x = CANVAS_W / 2;
    s_y = CANVAS_H / 2;
    s_ci = 0;
    s_nundo = 0;
    s_cleared = false;
    s_dirty = false;
    fill_bg();
    ESP_LOGI(TAG, "DMA free %u largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    s_cursor = lv_obj_create(root);
    lv_obj_set_size(s_cursor, BRUSH + 2, BRUSH + 2);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_cursor, 1, 0);
    lv_obj_set_style_radius(s_cursor, 0, 0);
    lv_obj_set_style_pad_all(s_cursor, 0, 0);
    s_swatch = ui_panel(root, 4, OS_ROOT_H - UI_HINT_H + 2, 12, 12);
    s_hint = ui_hint(root, "SELECT paint  CANCEL undo  L+R colour  U+D clear");
    (void)s_hint;
    place_cursor();
}

static void on_leave(void)
{
    if (s_fb && s_dirty) {
        // Pause LVGL so its DMA flush buffers sit idle; LCD and SD share SPI2.
        bsp_display_pause_lvgl(true);
        const char *msg = save_doodle(s_fb);
        bsp_display_pause_lvgl(false);
        os_toast("%s", msg);
    }
    free(s_fb);
    s_fb = NULL;
    free(s_undo);
    s_undo = NULL;
    s_canvas = s_cursor = s_swatch = s_hint = NULL;
    s_dirty = false;
}

static bool select_held(void)
{
    return bsp_buttons_get() & os_key_button(OS_KEY_SELECT);
}

static bool start_chord(void)
{
    uint32_t b = bsp_buttons_get();
    return (b & os_key_button(OS_KEY_UP)) && (b & os_key_button(OS_KEY_DOWN));
}

static bool color_chord(void)
{
    uint32_t b = bsp_buttons_get();
    return (b & os_key_button(OS_KEY_LEFT)) && (b & os_key_button(OS_KEY_RIGHT));
}

static void move_and_maybe_paint(int dx, int dy)
{
    int nx = s_x + dx * BRUSH, ny = s_y + dy * BRUSH;
    if (nx < 0) {
        nx = 0;
    }
    if (ny < 0) {
        ny = 0;
    }
    if (nx > CANVAS_W - BRUSH) {
        nx = CANVAS_W - BRUSH;
    }
    if (ny > CANVAS_H - BRUSH) {
        ny = CANVAS_H - BRUSH;
    }
    s_x = nx;
    s_y = ny;
    place_cursor();
    if (select_held()) {
        paint();
    }
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s_fb) {
        return false;
    }
    if ((k == OS_KEY_UP || k == OS_KEY_DOWN) && start_chord()) {
        if (!repeat && !s_cleared) {
            clear_canvas();
            s_cleared = true;
        }
        return true;
    }
    if ((k == OS_KEY_LEFT || k == OS_KEY_RIGHT) && color_chord()) {
        if (!repeat) {
            s_ci = (s_ci + 1) % NCOLORS;
            place_cursor();
        }
        return true;
    }
    switch (k) {
    case OS_KEY_LEFT:
        move_and_maybe_paint(-1, 0);
        return true;
    case OS_KEY_RIGHT:
        move_and_maybe_paint(1, 0);
        return true;
    case OS_KEY_UP:
        move_and_maybe_paint(0, -1);
        return true;
    case OS_KEY_DOWN:
        move_and_maybe_paint(0, 1);
        return true;
    case OS_KEY_SELECT:
        paint();
        return true;
    case OS_KEY_BACK:
        if (!repeat) {
            undo();
        }
        return true;
    default:
        return false;
    }
}

static void on_tick(void)
{
    if (!start_chord()) {
        s_cleared = false;
    }
}

const os_app_t app_paint = {
    .id = "paint",
    .name = "Paint",
    .symbol = LV_SYMBOL_EDIT,
    .accent = COL_PINK,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
