// Snake: classic grow-on-food for the BP26 badge (320×240).
// D-pad steers; SELECT starts / resumes; CANCEL pauses (play) or exits.
// LEFT/RIGHT on title set Easy / Normal / Hard (NVS). Wall death + self-hit.
// Visual theme: bright grass / fire / water palette + cream HUD (inspired by
// classic handheld RPG battle UI — original colors only; no franchise art).

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
#define NVS_DIFF        "diff"

#define HUD_H           22
#define GRID_COLS       20
#define GRID_ROWS       12
#define CELL            12              /* 240×144 playfield */
#define GRID_PX_W       (GRID_COLS * CELL)
#define GRID_PX_H       (GRID_ROWS * CELL)
#define FRAME_PAD       2
#define SNAKE_MAX       (GRID_COLS * GRID_ROWS)

#define TICKS_MIN       1
#define DIFF_N          3

/* Playfield palette — grass floor, water wall rim, fire food (type triad) */
#define PX_CHECK_A      0x58A838        /* bright grass A */
#define PX_CHECK_B      0x70C048        /* bright grass B */
#define PX_GRID         0x3D7A28        /* leaf veins */
#define PX_WALL         0x3B6FD8        /* water blue — wall death cue */
#define PX_BODY         0x38B030        /* grass body */
#define PX_BODY_DIM     0x248028        /* deeper leaf segments */
#define PX_HEAD         0xC8F040        /* sunny lime head */
#define PX_HEAD_EYE     0x1A2840        /* dark pupils on bright head */
#define PX_FOOD         0xF07028        /* fire orange */
#define PX_FOOD_CORE    0xF8D030        /* warm yellow core */
#define PX_DEAD_TINT    0x682018        /* fire-red wash on game over */

/* HUD / overlay — cream battle-box feel (original colours) */
#define UI_TITLE        0x38B030        /* grass */
#define UI_SCORE        0xE84828        /* fire */
#define UI_BEST         0x3B6FD8        /* water */
#define UI_BOX_BG       0xF8F0D8        /* parchment cream */
#define UI_BOX_INK      0x203060        /* navy dialogue ink */
#define UI_BOX_EDGE     0x2A58B0        /* water rim on message box */

typedef enum {
    ST_IDLE = 0,
    ST_PLAY,
    ST_PAUSE,
    ST_OVER,
} state_t;

typedef enum {
    DIFF_EASY = 0,
    DIFF_NORMAL = 1,
    DIFF_HARD = 2,
} diff_t;

typedef struct {
    int8_t x, y;
} cell_t;

static lv_obj_t *s_root, *s_title, *s_score_lab, *s_best_lab, *s_hint;
static lv_obj_t *s_frame, *s_canvas, *s_overlay;
static lv_image_dsc_t s_dsc;
static uint16_t *s_fb;
static state_t s_state;
static diff_t s_diff;
static cell_t s_body[SNAKE_MAX];
static int s_len;
static int8_t s_dx, s_dy;           /* current heading */
static int8_t s_ndx, s_ndy;         /* queued turn (applied on next step) */
static cell_t s_food;
static int s_score;
static int32_t s_best;
static int s_wait;                  /* ticks until next step */
static int s_step_period;

static const char *diff_name(diff_t d)
{
    switch (d) {
    case DIFF_EASY:   return "Easy";
    case DIFF_HARD:   return "Hard";
    default:          return "Normal";
    }
}

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

static void paint_head(int cx, int cy)
{
    paint_cell(cx, cy, PX_HEAD, 1);
    /* eyes toward current heading */
    int ex = cx * CELL + CELL / 2;
    int ey = cy * CELL + CELL / 2;
    int ox = s_dx * 2;
    int oy = s_dy * 2;
    int px = 2, py = 2;
    if (s_dx != 0) {
        px = 1;
        py = 2;
    } else if (s_dy != 0) {
        px = 2;
        py = 1;
    }
    fill_rect(ex + ox - px - 1, ey + oy - py, px, py, color_u16(PX_HEAD_EYE));
    fill_rect(ex + ox + 1, ey + oy - py, px, py, color_u16(PX_HEAD_EYE));
}

static void paint_food(int cx, int cy)
{
    /* fire-type orb: orange pad + yellow core — pops on grass floor */
    paint_cell(cx, cy, PX_FOOD, 1);
    paint_cell(cx, cy, PX_FOOD_CORE, 4);
}

// ---- NVS ------------------------------------------------------------------

static void prefs_load(void)
{
    s_best = 0;
    s_diff = DIFF_NORMAL;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, NVS_BEST, &v) == ESP_OK && v > 0) {
        s_best = v;
    }
    v = 0;
    if (nvs_get_i32(h, NVS_DIFF, &v) == ESP_OK && v >= 0 && v < DIFF_N) {
        s_diff = (diff_t)v;
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

static void diff_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_DIFF, (int32_t)s_diff);
    nvs_commit(h);
    nvs_close(h);
}

// ---- HUD / overlay --------------------------------------------------------

static void refresh_hud(void)
{
    if (s_title) {
        lv_label_set_text_fmt(s_title, "Snake · %s", diff_name(s_diff));
    }
    if (s_score_lab) {
        lv_label_set_text_fmt(s_score_lab, "SCORE  %d", s_score);
    }
    if (s_best_lab) {
        lv_label_set_text_fmt(s_best_lab, "BEST  %ld", (long)s_best);
    }
}

static void set_hint(const char *msg)
{
    if (s_hint) {
        lv_label_set_text(s_hint, msg);
    }
}

static void overlay_show(bool on, const char *title, const char *sub)
{
    if (!s_overlay) {
        return;
    }
    if (!on) {
        lv_obj_set_hidden(s_overlay, true);
        return;
    }
    lv_obj_set_hidden(s_overlay, false);
    lv_obj_t *t = lv_obj_get_child(s_overlay, 0);
    lv_obj_t *s = lv_obj_get_child(s_overlay, 1);
    if (t) {
        lv_label_set_text(t, title ? title : "");
    }
    if (s) {
        lv_label_set_text(s, sub ? sub : "");
    }
}

// ---- draw -----------------------------------------------------------------

static void redraw(void)
{
    if (!s_fb) {
        return;
    }

    uint16_t a = color_u16(PX_CHECK_A);
    uint16_t b = color_u16(PX_CHECK_B);
    uint16_t grid = color_u16(PX_GRID);
    uint16_t wall = color_u16(PX_WALL);

    /* checkerboard floor */
    for (int cy = 0; cy < GRID_ROWS; cy++) {
        for (int cx = 0; cx < GRID_COLS; cx++) {
            uint16_t c = ((cx + cy) & 1) ? a : b;
            fill_rect(cx * CELL, cy * CELL, CELL, CELL, c);
        }
    }

    /* subtle inner grid */
    for (int c = 1; c < GRID_COLS; c++) {
        int x = c * CELL;
        for (int y = 0; y < GRID_PX_H; y++) {
            s_fb[y * GRID_PX_W + x] = grid;
        }
    }
    for (int r = 1; r < GRID_ROWS; r++) {
        int y = r * CELL;
        for (int x = 0; x < GRID_PX_W; x++) {
            s_fb[y * GRID_PX_W + x] = grid;
        }
    }

    /* 2 px wall rim — visual cue that edges kill */
    fill_rect(0, 0, GRID_PX_W, 2, wall);
    fill_rect(0, GRID_PX_H - 2, GRID_PX_W, 2, wall);
    fill_rect(0, 0, 2, GRID_PX_H, wall);
    fill_rect(GRID_PX_W - 2, 0, 2, GRID_PX_H, wall);

    if (s_food.x >= 0) {
        paint_food(s_food.x, s_food.y);
    }

    for (int i = s_len - 1; i >= 1; i--) {
        uint32_t c = (i & 1) ? PX_BODY : PX_BODY_DIM;
        paint_cell(s_body[i].x, s_body[i].y, c, 1);
    }
    if (s_len > 0) {
        paint_head(s_body[0].x, s_body[0].y);
    }

    /* soft wash on game over so overlay text stays readable */
    if (s_state == ST_OVER) {
        uint16_t tint = color_u16(PX_DEAD_TINT);
        for (int i = 0; i < GRID_PX_W * GRID_PX_H; i++) {
            /* cheap darken: blend toward tint by keeping half the bits */
            uint16_t p = s_fb[i];
            s_fb[i] = (uint16_t)(((p & 0xF7DE) >> 1) + ((tint & 0xF7DE) >> 1));
        }
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
    for (int tries = 0; tries < 200; tries++) {
        int x = (int)(esp_random() % GRID_COLS);
        int y = (int)(esp_random() % GRID_ROWS);
        if (!occupied(x, y)) {
            s_food.x = (int8_t)x;
            s_food.y = (int8_t)y;
            return;
        }
    }
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

/* Easy: start 5 ticks / accel every 7 pts. Normal: 3 / 5. Hard: 2 / 3. */
static void update_speed(void)
{
    int start, every;
    switch (s_diff) {
    case DIFF_EASY:
        start = 5;
        every = 7;
        break;
    case DIFF_HARD:
        start = 2;
        every = 3;
        break;
    default:
        start = 3;
        every = 5;
        break;
    }
    int p = start - s_score / every;
    s_step_period = p < TICKS_MIN ? TICKS_MIN : p;
}

static void idle_overlay(void)
{
    char sub[40];
    snprintf(sub, sizeof(sub), "%s — SELECT start", diff_name(s_diff));
    overlay_show(true, "SNAKE", sub);
    set_hint("L/R difficulty  SELECT start  CANCEL exit");
}

static void go_idle(void)
{
    s_state = ST_IDLE;
    refresh_hud();
    idle_overlay();
    /* preview snake */
    s_len = 4;
    s_body[0] = (cell_t){ 9, 6 };
    s_body[1] = (cell_t){ 8, 6 };
    s_body[2] = (cell_t){ 7, 6 };
    s_body[3] = (cell_t){ 6, 6 };
    s_dx = 1;
    s_dy = 0;
    s_food = (cell_t){ 14, 6 };
    redraw();
}

static void go_pause(void)
{
    s_state = ST_PAUSE;
    overlay_show(true, "PAUSED", "SELECT resume");
    set_hint("SELECT resume  CANCEL title");
    redraw();
}

static void resume_play(void)
{
    s_state = ST_PLAY;
    overlay_show(false, NULL, NULL);
    set_hint("D-pad steer  CANCEL pause");
    redraw();
}

static void game_over(void)
{
    s_state = ST_OVER;
    best_save();
    refresh_hud();
    char sub[40];
    snprintf(sub, sizeof(sub), "score %d — SELECT again", s_score);
    overlay_show(true, "GAME OVER", sub);
    set_hint("SELECT again  CANCEL exit");
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
    refresh_hud();
    overlay_show(false, NULL, NULL);
    set_hint("D-pad steer  CANCEL pause");
    redraw();
}

static void cycle_diff(int dir)
{
    int d = (int)s_diff + dir;
    if (d < 0) {
        d = DIFF_N - 1;
    } else if (d >= DIFF_N) {
        d = 0;
    }
    s_diff = (diff_t)d;
    diff_save();
    idle_overlay();
}

static void step(void)
{
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
        refresh_hud();
    }

    for (int i = s_len - 1; i > 0; i--) {
        s_body[i] = s_body[i - 1];
    }
    s_body[0].x = (int8_t)nx;
    s_body[0].y = (int8_t)ny;

    if (grow) {
        place_food();
        if (s_food.x < 0) {
            best_save();
            s_state = ST_OVER;
            refresh_hud();
            char sub[32];
            snprintf(sub, sizeof(sub), "score %d — SELECT again", s_score);
            overlay_show(true, "YOU WIN!", sub);
            set_hint("SELECT again  CANCEL exit");
            os_toast("Perfect — %d", s_score);
        }
    }

    redraw();
}

static void queue_dir(int8_t dx, int8_t dy)
{
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
    s_overlay = NULL;
    s_frame = NULL;

    prefs_load();

    /* compact top HUD — grass title / fire SCORE / water BEST */
    s_title = ui_label(root, &sol_font_20, UI_TITLE, "Snake");
    lv_obj_set_pos(s_title, 8, 2);

    s_score_lab = ui_label(root, &sol_mono_12, UI_SCORE, "");
    lv_obj_set_pos(s_score_lab, 100, 6);

    s_best_lab = ui_label(root, &sol_mono_12, UI_BEST, "");
    lv_obj_set_pos(s_best_lab, 210, 6);
    refresh_hud();

    /* thin type triad stripe under HUD (grass → fire → water) */
    {
        const int sy = HUD_H - 3;
        lv_obj_t *g = lv_obj_create(root);
        lv_obj_remove_style_all(g);
        lv_obj_set_size(g, 100, 3);
        lv_obj_set_pos(g, 8, sy);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g, lv_color_hex(UI_TITLE), 0);
        lv_obj_t *f = lv_obj_create(root);
        lv_obj_remove_style_all(f);
        lv_obj_set_size(f, 100, 3);
        lv_obj_set_pos(f, 110, sy);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(f, lv_color_hex(UI_SCORE), 0);
        lv_obj_t *w = lv_obj_create(root);
        lv_obj_remove_style_all(w);
        lv_obj_set_size(w, 100, 3);
        lv_obj_set_pos(w, 212, sy);
        lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(w, lv_color_hex(UI_BEST), 0);
    }

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
    int gy = HUD_H + 2;

    s_frame = ui_panel(root, gx - FRAME_PAD, gy - FRAME_PAD,
                       GRID_PX_W + 2 * FRAME_PAD, GRID_PX_H + 2 * FRAME_PAD);
    lv_obj_set_style_pad_all(s_frame, 0, 0);
    lv_obj_set_style_bg_color(s_frame, lv_color_hex(PX_WALL), 0);
    lv_obj_set_style_border_width(s_frame, 0, 0);

    s_canvas = lv_image_create(root);
    lv_image_set_src(s_canvas, &s_dsc);
    lv_obj_set_pos(s_canvas, gx, gy);

    /* cream message box overlay (handheld RPG dialogue vibe) */
    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, GRID_PX_W - 16, 52);
    lv_obj_set_pos(s_overlay, gx + 8, gy + (GRID_PX_H - 52) / 2);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(UI_BOX_BG), 0);
    lv_obj_set_style_border_width(s_overlay, 3, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(UI_BOX_EDGE), 0);
    lv_obj_set_style_radius(s_overlay, 6, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_t *ot = ui_label(s_overlay, &sol_font_20, UI_BOX_INK, "");
    lv_obj_set_width(ot, GRID_PX_W - 16);
    lv_obj_set_style_text_align(ot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *os = ui_label(s_overlay, &sol_mono_12, UI_BEST, "");
    lv_obj_set_width(os, GRID_PX_W - 16);
    lv_obj_set_style_text_align(os, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(os, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_hint = ui_hint(root, "");
    go_idle();
}

static void leave(void)
{
    free(s_fb);
    s_fb = NULL;
    s_root = NULL;
    s_title = NULL;
    s_score_lab = NULL;
    s_best_lab = NULL;
    s_hint = NULL;
    s_frame = NULL;
    s_canvas = NULL;
    s_overlay = NULL;
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

    if (s_state == ST_IDLE) {
        if (k == OS_KEY_SELECT) {
            start_game();
            return true;
        }
        if (k == OS_KEY_LEFT) {
            cycle_diff(-1);
            return true;
        }
        if (k == OS_KEY_RIGHT) {
            cycle_diff(1);
            return true;
        }
        if (k == OS_KEY_BACK) {
            return false; /* exit */
        }
        return true;
    }

    if (s_state == ST_OVER) {
        if (k == OS_KEY_SELECT) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK) {
            return false; /* exit */
        }
        return true;
    }

    if (s_state == ST_PAUSE) {
        if (k == OS_KEY_SELECT) {
            resume_play();
            return true;
        }
        if (k == OS_KEY_BACK) {
            best_save();
            go_idle();
            return true;
        }
        return true;
    }

    if (s_state == ST_PLAY) {
        if (k == OS_KEY_BACK) {
            go_pause();
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
    .accent = COL_LIME,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
