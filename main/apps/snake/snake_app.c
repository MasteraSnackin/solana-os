// Snake: classic grow-on-food for the BP26 badge.
// D-pad steers; SELECT starts / restarts; CANCEL exits from idle / game over.
// Wall death (and self-hit) — the playfield does not wrap. Offline; NVS best.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "nvs.h"

#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"

#define NVS_NS          "snake"
#define NVS_BEST        "best"

#define HDR_H           28
#define GRID_COLS       20
#define GRID_ROWS       12
#define CELL            12              /* 240×144 playfield; leaves room for status + hint */
#define GRID_PX_W       (GRID_COLS * CELL)
#define GRID_PX_H       (GRID_ROWS * CELL)
#define SNAKE_MAX       (GRID_COLS * GRID_ROWS)

#define TICKS_START     3       /* move every N × OS_TICK_MS (100 ms) */
#define TICKS_MIN       1

typedef enum {
    ST_IDLE = 0,
    ST_PLAY,
    ST_OVER,
} state_t;

typedef struct {
    int8_t x, y;
} cell_t;

static lv_obj_t *s_root, *s_title, *s_meta, *s_status, *s_hint, *s_canvas;
static lv_image_dsc_t s_dsc;
static uint16_t *s_fb;
static state_t s_state;
static cell_t s_body[SNAKE_MAX];
static int s_len;
static int8_t s_dx, s_dy;           /* current heading */
static int8_t s_ndx, s_ndy;         /* queued turn (applied on next step) */
static cell_t s_food;
static int s_score;
static int32_t s_best;
static int s_wait;                  /* ticks until next step */
static int s_step_period;

// ---- colour helpers -------------------------------------------------------

static uint16_t color_u16(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t c)
{
    if (!s_fb) {
        return;
    }
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= GRID_PX_H) {
            continue;
        }
        uint16_t *row = &s_fb[y * GRID_PX_W];
        for (int x = x0; x < x0 + w; x++) {
            if (x >= 0 && x < GRID_PX_W) {
                row[x] = c;
            }
        }
    }
}

static void paint_cell(int cx, int cy, uint32_t rgb, int inset)
{
    int x = cx * CELL + inset;
    int y = cy * CELL + inset;
    int s = CELL - 2 * inset;
    if (s < 1) {
        s = 1;
    }
    fill_rect(x, y, s, s, color_u16(rgb));
}

// ---- NVS ------------------------------------------------------------------

static void best_load(void)
{
    s_best = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, NVS_BEST, &v) == ESP_OK && v > 0) {
        s_best = v;
    }
    nvs_close(h);
}

static void best_save(void)
{
    if (s_score <= s_best) {
        return;
    }
    s_best = s_score;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_BEST, s_best);
    nvs_commit(h);
    nvs_close(h);
}

// ---- draw -----------------------------------------------------------------

static void refresh_meta(void)
{
    if (!s_meta) {
        return;
    }
    lv_label_set_text_fmt(s_meta, "score %d   best %ld", s_score, (long)s_best);
}

static void set_status(const char *msg)
{
    if (s_status) {
        lv_label_set_text(s_status, msg);
    }
}

static void redraw(void)
{
    if (!s_fb) {
        return;
    }
    /* playfield + 1 px grid lines */
    uint16_t bg = color_u16(COL_PANEL);
    uint16_t line = color_u16(COL_BORDER);
    for (int i = 0; i < GRID_PX_W * GRID_PX_H; i++) {
        s_fb[i] = bg;
    }
    for (int c = 0; c <= GRID_COLS; c++) {
        int x = c * CELL;
        if (x >= GRID_PX_W) {
            x = GRID_PX_W - 1;
        }
        for (int y = 0; y < GRID_PX_H; y++) {
            s_fb[y * GRID_PX_W + x] = line;
        }
    }
    for (int r = 0; r <= GRID_ROWS; r++) {
        int y = r * CELL;
        if (y >= GRID_PX_H) {
            y = GRID_PX_H - 1;
        }
        for (int x = 0; x < GRID_PX_W; x++) {
            s_fb[y * GRID_PX_W + x] = line;
        }
    }

    paint_cell(s_food.x, s_food.y, COL_ORANGE, 2);
    for (int i = s_len - 1; i >= 0; i--) {
        uint32_t c = (i == 0) ? COL_GREEN : COL_LIME;
        paint_cell(s_body[i].x, s_body[i].y, c, 2);
    }

    if (s_canvas) {
        lv_obj_invalidate(s_canvas);
    }
}

// ---- game -----------------------------------------------------------------

static bool occupied(int x, int y)
{
    for (int i = 0; i < s_len; i++) {
        if (s_body[i].x == x && s_body[i].y == y) {
            return true;
        }
    }
    return false;
}

static void place_food(void)
{
    if (s_len >= SNAKE_MAX) {
        s_food.x = -1;
        s_food.y = -1;
        return;
    }
    /* rejection sampling; grid is small */
    for (int tries = 0; tries < 200; tries++) {
        int x = (int)(esp_random() % GRID_COLS);
        int y = (int)(esp_random() % GRID_ROWS);
        if (!occupied(x, y)) {
            s_food.x = (int8_t)x;
            s_food.y = (int8_t)y;
            return;
        }
    }
    /* dense: linear scan */
    for (int y = 0; y < GRID_ROWS; y++) {
        for (int x = 0; x < GRID_COLS; x++) {
            if (!occupied(x, y)) {
                s_food.x = (int8_t)x;
                s_food.y = (int8_t)y;
                return;
            }
        }
    }
    s_food.x = -1;
    s_food.y = -1;
}

static void update_speed(void)
{
    /* every 5 points shave one tick period, floor at TICKS_MIN */
    int p = TICKS_START - s_score / 5;
    s_step_period = p < TICKS_MIN ? TICKS_MIN : p;
}

static void game_over(void)
{
    s_state = ST_OVER;
    best_save();
    refresh_meta();
    set_status("Game over");
    if (s_hint) {
        lv_label_set_text(s_hint, "SELECT again  CANCEL exit");
    }
    os_toast("Game over — %d", s_score);
    redraw();
}

static void start_game(void)
{
    s_len = 3;
    s_body[0] = (cell_t){ GRID_COLS / 2, GRID_ROWS / 2 };
    s_body[1] = (cell_t){ (int8_t)(s_body[0].x - 1), s_body[0].y };
    s_body[2] = (cell_t){ (int8_t)(s_body[0].x - 2), s_body[0].y };
    s_dx = 1;
    s_dy = 0;
    s_ndx = 1;
    s_ndy = 0;
    s_score = 0;
    s_state = ST_PLAY;
    update_speed();
    s_wait = s_step_period;
    place_food();
    refresh_meta();
    set_status("Go!");
    if (s_hint) {
        lv_label_set_text(s_hint, "D-pad steer  CANCEL pause");
    }
    redraw();
}

static void step(void)
{
    /* apply queued turn; ignore 180° reverse */
    if (!(s_ndx == -s_dx && s_ndy == -s_dy)) {
        s_dx = s_ndx;
        s_dy = s_ndy;
    }

    int nx = s_body[0].x + s_dx;
    int ny = s_body[0].y + s_dy;

    /* wall death — no wrap */
    if (nx < 0 || ny < 0 || nx >= GRID_COLS || ny >= GRID_ROWS) {
        game_over();
        return;
    }

    /* self-hit: allow sliding into the vacating tail unless growing */
    bool grow = (s_food.x == nx && s_food.y == ny);
    int check_len = grow ? s_len : s_len - 1;
    for (int i = 0; i < check_len; i++) {
        if (s_body[i].x == nx && s_body[i].y == ny) {
            game_over();
            return;
        }
    }

    if (grow) {
        if (s_len < SNAKE_MAX) {
            s_len++;
        }
        s_score++;
        best_save();
        update_speed();
        refresh_meta();
    }

    for (int i = s_len - 1; i > 0; i--) {
        s_body[i] = s_body[i - 1];
    }
    s_body[0].x = (int8_t)nx;
    s_body[0].y = (int8_t)ny;

    if (grow) {
        place_food();
        if (s_food.x < 0) {
            /* filled the board */
            best_save();
            s_state = ST_OVER;
            set_status("You win!");
            if (s_hint) {
                lv_label_set_text(s_hint, "SELECT again  CANCEL exit");
            }
            os_toast("Perfect — %d", s_score);
        }
    }

    redraw();
}

static void queue_dir(int8_t dx, int8_t dy)
{
    /* only one turn per step; reject immediate reverse vs current heading */
    if (dx == -s_dx && dy == -s_dy) {
        return;
    }
    s_ndx = dx;
    s_ndy = dy;
}

// ---- lifecycle ------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_state = ST_IDLE;
    s_len = 0;
    s_score = 0;
    s_wait = 0;
    s_dx = s_dy = s_ndx = s_ndy = 0;
    s_food.x = s_food.y = -1;
    s_fb = NULL;
    s_canvas = NULL;

    best_load();

    s_title = ui_label(root, &sol_font_20, COL_GREEN, "Snake");
    lv_obj_set_pos(s_title, 10, 4);

    s_meta = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_meta, 10, 26);
    lv_obj_set_width(s_meta, OS_ROOT_W - 20);
    refresh_meta();

    size_t bytes = (size_t)GRID_PX_W * GRID_PX_H * 2;
    s_fb = heap_caps_malloc(bytes, os_mem_bulk_caps());
    if (!s_fb) {
        s_fb = malloc(bytes);
    }
    if (!s_fb) {
        ui_label(root, &sol_font_14, COL_ERR, "Not enough memory");
        s_hint = ui_hint(root, "CANCEL exit");
        return;
    }

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = GRID_PX_W;
    s_dsc.header.h = GRID_PX_H;
    s_dsc.header.stride = GRID_PX_W * 2;
    s_dsc.data_size = (uint32_t)bytes;
    s_dsc.data = (const uint8_t *)s_fb;

    int gx = (OS_ROOT_W - GRID_PX_W) / 2;
    int gy = HDR_H + 4;
    s_canvas = lv_image_create(root);
    lv_image_set_src(s_canvas, &s_dsc);
    lv_obj_set_pos(s_canvas, gx, gy);

    /* seed a preview snake so idle isn't an empty board */
    s_len = 3;
    s_body[0] = (cell_t){ 8, 6 };
    s_body[1] = (cell_t){ 7, 6 };
    s_body[2] = (cell_t){ 6, 6 };
    s_food = (cell_t){ 12, 6 };
    redraw();

    int status_y = gy + GRID_PX_H + 2;
    s_status = ui_label(root, &sol_font_14, COL_TEXT, "SELECT to start");
    lv_obj_set_pos(s_status, 10, status_y);
    lv_obj_set_width(s_status, OS_ROOT_W - 20);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    s_hint = ui_hint(root, "SELECT start  D-pad steer  CANCEL exit");
}

static void leave(void)
{
    free(s_fb);
    s_fb = NULL;
    s_root = NULL;
    s_title = NULL;
    s_meta = NULL;
    s_status = NULL;
    s_hint = NULL;
    s_canvas = NULL;
}

static void on_tick(void)
{
    if (s_state != ST_PLAY) {
        return;
    }
    if (s_wait > 0) {
        s_wait--;
        if (s_wait > 0) {
            return;
        }
    }
    step();
    s_wait = s_step_period;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (repeat) {
        return true;
    }

    if (s_state == ST_IDLE || s_state == ST_OVER) {
        if (k == OS_KEY_SELECT) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK) {
            return false; /* exit */
        }
        return true;
    }

    if (s_state == ST_PLAY) {
        if (k == OS_KEY_BACK) {
            s_state = ST_IDLE;
            best_save();
            refresh_meta();
            set_status("SELECT to start");
            if (s_hint) {
                lv_label_set_text(s_hint, "SELECT start  D-pad steer  CANCEL exit");
            }
            return true;
        }
        if (k == OS_KEY_UP) {
            queue_dir(0, -1);
        } else if (k == OS_KEY_DOWN) {
            queue_dir(0, 1);
        } else if (k == OS_KEY_LEFT) {
            queue_dir(-1, 0);
        } else if (k == OS_KEY_RIGHT) {
            queue_dir(1, 0);
        }
        return true;
    }

    return false;
}

const os_app_t app_snake = {
    .id = "snake",
    .name = "Snake",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
