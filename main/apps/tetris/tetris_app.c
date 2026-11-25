// Classic Tetris for the BP26 badge (320×240, Solana OS).
// Seven tetrominoes, rotate, soft drop, line clear, score / level speed-up.
// Offline — no Wi-Fi, mesh, or SD. Best score kept in NVS.

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"

#include "core/os.h"
#include "core/ui.h"

#define NVS_NS          "tetris"
#define NVS_BEST        "best"

#define COLS            10
#define ROWS            20
#define CELL            10
#define BOARD_W         (COLS * CELL)
#define BOARD_H         (ROWS * CELL)
#define BOARD_X         8
#define BOARD_Y         2

#define PIECE_N         7
#define BAG_N           7

typedef enum {
    ST_TITLE = 0,
    ST_PLAY,
    ST_PAUSE,
    ST_OVER,
} state_t;

// Each rotation is 4 cells as (dx,dy) within a 4×4 box (origin top-left).
typedef struct {
    int8_t x, y;
} cell_t;

typedef struct {
    cell_t rot[4][4];
    uint32_t color;
} piece_def_t;

// I J L O S T Z — classic orientations (0 = spawn).
static const piece_def_t k_pieces[PIECE_N] = {
    { /* I — cyan */
        {
            {{0, 1}, {1, 1}, {2, 1}, {3, 1}},
            {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
            {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
            {{1, 0}, {1, 1}, {1, 2}, {1, 3}},
        },
        COL_AQUA,
    },
    { /* J — blue */
        {
            {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
            {{1, 0}, {1, 1}, {0, 2}, {1, 2}},
        },
        COL_BLUE,
    },
    { /* L — orange */
        {
            {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
            {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
        },
        COL_ORANGE,
    },
    { /* O — yellow */
        {
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        },
        COL_YELLOW,
    },
    { /* S — green */
        {
            {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
            {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
            {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
            {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
        },
        COL_GREEN,
    },
    { /* T — purple */
        {
            {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
            {{1, 0}, {0, 1}, {1, 1}, {1, 2}},
        },
        COL_PURPLE,
    },
    { /* Z — pink/red */
        {
            {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
            {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
            {{1, 0}, {0, 1}, {1, 1}, {0, 2}},
        },
        COL_PINK,
    },
};

static lv_obj_t *s_root, *s_board_bg, *s_hint;
static lv_obj_t *s_cell[ROWS][COLS];
static lv_obj_t *s_score, *s_level, *s_lines, *s_best_lab, *s_status;
static lv_obj_t *s_next_cell[4][4];
static lv_obj_t *s_overlay;

static state_t s_state;
static uint8_t s_grid[ROWS][COLS];          /* 0 empty, 1..7 piece id */
static int s_px, s_py, s_prot, s_pid;       /* active piece */
static int s_next;
static int s_bag[BAG_N], s_bag_i;
static int32_t s_score_v, s_best;
static int s_level_v, s_lines_v;
static int s_fall;                          /* ticks until gravity */

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
    if (s_score_v <= s_best) {
        return;
    }
    s_best = s_score_v;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_BEST, s_best);
    nvs_commit(h);
    nvs_close(h);
}

// ---- bag / RNG ------------------------------------------------------------

static void bag_refill(void)
{
    for (int i = 0; i < BAG_N; i++) {
        s_bag[i] = i;
    }
    for (int i = BAG_N - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        int t = s_bag[i];
        s_bag[i] = s_bag[j];
        s_bag[j] = t;
    }
    s_bag_i = 0;
}

static int bag_next(void)
{
    if (s_bag_i >= BAG_N) {
        bag_refill();
    }
    return s_bag[s_bag_i++];
}

// ---- collision ------------------------------------------------------------

static bool cell_ok(int x, int y)
{
    if (x < 0 || x >= COLS || y >= ROWS) {
        return false;
    }
    if (y < 0) {
        return true;    /* allow spawn above the visible board */
    }
    return s_grid[y][x] == 0;
}

static bool fits(int id, int rot, int ox, int oy)
{
    const piece_def_t *p = &k_pieces[id];
    for (int i = 0; i < 4; i++) {
        int x = ox + p->rot[rot][i].x;
        int y = oy + p->rot[rot][i].y;
        if (!cell_ok(x, y)) {
            return false;
        }
    }
    return true;
}

static void lock_piece(void)
{
    const piece_def_t *p = &k_pieces[s_pid];
    for (int i = 0; i < 4; i++) {
        int x = s_px + p->rot[s_prot][i].x;
        int y = s_py + p->rot[s_prot][i].y;
        if (y >= 0 && y < ROWS && x >= 0 && x < COLS) {
            s_grid[y][x] = (uint8_t)(s_pid + 1);
        }
    }
}

// ---- lines / score --------------------------------------------------------

static int clear_lines(void)
{
    int cleared = 0;
    for (int y = ROWS - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < COLS; x++) {
            if (!s_grid[y][x]) {
                full = false;
                break;
            }
        }
        if (!full) {
            continue;
        }
        cleared++;
        for (int yy = y; yy > 0; yy--) {
            memcpy(s_grid[yy], s_grid[yy - 1], COLS);
        }
        memset(s_grid[0], 0, COLS);
        y++;    /* re-check this row after shift */
    }
    return cleared;
}

static void add_score(int cleared)
{
    static const int k_pts[] = {0, 40, 100, 300, 1200};
    if (cleared < 1) {
        return;
    }
    if (cleared > 4) {
        cleared = 4;
    }
    s_score_v += (int32_t)k_pts[cleared] * (int32_t)s_level_v;
    s_lines_v += cleared;
    int new_level = 1 + s_lines_v / 10;
    if (new_level > 15) {
        new_level = 15;
    }
    if (new_level > s_level_v) {
        s_level_v = new_level;
    }
}

// Gravity interval in on_tick counts (OS_TICK_MS = 100).
static int fall_ticks(void)
{
    int t = 11 - s_level_v;     /* L1=10 → 1.0s, L10=1 → 0.1s */
    if (t < 1) {
        t = 1;
    }
    return t;
}

// ---- spawn ----------------------------------------------------------------

static void spawn_piece(void)
{
    s_pid = s_next;
    s_next = bag_next();
    s_prot = 0;
    s_px = 3;
    s_py = -1;      /* I/J/L/T sit with row 1 on the top */
    if (s_pid == 0) {
        s_py = -1;  /* I */
    }
    s_fall = fall_ticks();
}

static bool spawn_ok(void)
{
    return fits(s_pid, s_prot, s_px, s_py);
}

// ---- paint ----------------------------------------------------------------

static uint32_t color_for(uint8_t id)
{
    if (id == 0 || id > PIECE_N) {
        return COL_PANEL;
    }
    return k_pieces[id - 1].color;
}

static void paint_cell(lv_obj_t *obj, uint32_t c, bool empty)
{
    if (!obj) {
        return;
    }
    if (empty) {
        lv_obj_set_style_bg_color(obj, ui_col(COL_BG), 0);
        lv_obj_set_style_border_color(obj, ui_col(COL_BORDER), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
    } else {
        lv_obj_set_style_bg_color(obj, lv_color_hex(c), 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(c), 0);
        lv_obj_set_style_border_width(obj, 0, 0);
    }
}

static void paint_board(void)
{
    uint8_t view[ROWS][COLS];
    memcpy(view, s_grid, sizeof(view));

    if (s_state == ST_PLAY || s_state == ST_PAUSE) {
        const piece_def_t *p = &k_pieces[s_pid];
        for (int i = 0; i < 4; i++) {
            int x = s_px + p->rot[s_prot][i].x;
            int y = s_py + p->rot[s_prot][i].y;
            if (y >= 0 && y < ROWS && x >= 0 && x < COLS) {
                view[y][x] = (uint8_t)(s_pid + 1);
            }
        }
    }

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            uint8_t id = view[y][x];
            paint_cell(s_cell[y][x], color_for(id), id == 0);
        }
    }
}

static void paint_next(void)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            paint_cell(s_next_cell[y][x], COL_PANEL, true);
        }
    }
    if (s_state == ST_TITLE) {
        return;
    }
    const piece_def_t *p = &k_pieces[s_next];
    for (int i = 0; i < 4; i++) {
        int x = p->rot[0][i].x;
        int y = p->rot[0][i].y;
        if (x >= 0 && x < 4 && y >= 0 && y < 4) {
            paint_cell(s_next_cell[y][x], p->color, false);
        }
    }
}

static void refresh_hud(void)
{
    if (s_score) {
        lv_label_set_text_fmt(s_score, "SCORE  %ld", (long)s_score_v);
    }
    if (s_level) {
        lv_label_set_text_fmt(s_level, "LEVEL  %d", s_level_v);
    }
    if (s_lines) {
        lv_label_set_text_fmt(s_lines, "LINES  %d", s_lines_v);
    }
    if (s_best_lab) {
        lv_label_set_text_fmt(s_best_lab, "BEST   %ld", (long)s_best);
    }
}

static void set_status(const char *msg)
{
    if (s_status) {
        lv_label_set_text(s_status, msg ? msg : "");
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

// ---- game flow ------------------------------------------------------------

static void game_reset_board(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_score_v = 0;
    s_level_v = 1;
    s_lines_v = 0;
    bag_refill();
    s_next = bag_next();
    spawn_piece();
}

static void go_title(void)
{
    s_state = ST_TITLE;
    memset(s_grid, 0, sizeof(s_grid));
    paint_board();
    paint_next();
    refresh_hud();
    set_status("");
    overlay_show(true, "TETRIS", "SELECT start");
    set_hint("SELECT start  CANCEL exit");
}

static void go_play(void)
{
    s_state = ST_PLAY;
    overlay_show(false, NULL, NULL);
    set_status("");
    set_hint("L/R move  UP/SELECT rot  DOWN drop");
    paint_board();
    paint_next();
    refresh_hud();
}

static void go_pause(void)
{
    s_state = ST_PAUSE;
    overlay_show(true, "PAUSED", "SELECT resume");
    set_hint("SELECT resume  CANCEL title");
}

static void go_over(void)
{
    s_state = ST_OVER;
    best_save();
    refresh_hud();
    overlay_show(true, "GAME OVER", "SELECT again");
    set_hint("SELECT again  CANCEL title");
    os_toast("Game over — %ld", (long)s_score_v);
}

static void start_game(void)
{
    game_reset_board();
    if (!spawn_ok()) {
        /* extremely unlikely on empty board */
        go_over();
        return;
    }
    go_play();
}

static void soft_drop_step(void)
{
    if (fits(s_pid, s_prot, s_px, s_py + 1)) {
        s_py++;
        s_score_v += 1;     /* soft-drop bonus */
        s_fall = fall_ticks();
        paint_board();
        refresh_hud();
    }
}

static void gravity_tick(void)
{
    if (fits(s_pid, s_prot, s_px, s_py + 1)) {
        s_py++;
        paint_board();
        return;
    }
    /* lock */
    lock_piece();
    int n = clear_lines();
    if (n) {
        add_score(n);
    }
    refresh_hud();
    spawn_piece();
    paint_next();
    if (!spawn_ok()) {
        paint_board();
        go_over();
        return;
    }
    paint_board();
}

static bool try_move(int dx, int dy)
{
    if (!fits(s_pid, s_prot, s_px + dx, s_py + dy)) {
        return false;
    }
    s_px += dx;
    s_py += dy;
    paint_board();
    return true;
}

static bool try_rotate(void)
{
    int nrot = (s_prot + 1) & 3;
    static const int kicks[] = {0, -1, 1, -2, 2};
    for (int i = 0; i < 5; i++) {
        if (fits(s_pid, nrot, s_px + kicks[i], s_py)) {
            s_prot = nrot;
            s_px += kicks[i];
            paint_board();
            return true;
        }
    }
    return false;
}

// ---- lifecycle ------------------------------------------------------------

static lv_obj_t *make_cell(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 1, 0);
    lv_obj_set_scrollable(c, false);
    paint_cell(c, COL_BG, true);
    return c;
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    best_load();

    s_board_bg = ui_panel(root, BOARD_X - 2, BOARD_Y - 2, BOARD_W + 4, BOARD_H + 4);
    lv_obj_set_style_pad_all(s_board_bg, 0, 0);
    lv_obj_set_style_bg_color(s_board_bg, ui_col(COL_BORDER), 0);

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            s_cell[y][x] = make_cell(root, BOARD_X + x * CELL, BOARD_Y + y * CELL, CELL - 1);
        }
    }

    int hx = BOARD_X + BOARD_W + 14;
    lv_obj_t *hud = ui_label(root, &sol_font_20, COL_GREEN, "Tetris");
    lv_obj_set_pos(hud, hx, 4);

    s_score = ui_label(root, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_pos(s_score, hx, 32);
    lv_obj_set_width(s_score, OS_ROOT_W - hx - 8);

    s_level = ui_label(root, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_pos(s_level, hx, 50);
    lv_obj_set_width(s_level, OS_ROOT_W - hx - 8);

    s_lines = ui_label(root, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_pos(s_lines, hx, 68);
    lv_obj_set_width(s_lines, OS_ROOT_W - hx - 8);

    s_best_lab = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_best_lab, hx, 90);
    lv_obj_set_width(s_best_lab, OS_ROOT_W - hx - 8);

    lv_obj_t *next_lab = ui_label(root, &sol_mono_12, COL_DIM, "NEXT");
    lv_obj_set_pos(next_lab, hx, 118);

    int nx = hx;
    int ny = 136;
    lv_obj_t *next_bg = ui_panel(root, nx - 2, ny - 2, 4 * 12 + 4, 4 * 12 + 4);
    lv_obj_set_style_pad_all(next_bg, 0, 0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            s_next_cell[y][x] = make_cell(root, nx + x * 12, ny + y * 12, 11);
        }
    }

    s_status = ui_label(root, &sol_mono_12, COL_YELLOW, "");
    lv_obj_set_pos(s_status, hx, BOARD_Y + BOARD_H - 14);
    lv_obj_set_width(s_status, OS_ROOT_W - hx - 8);

    /* centered overlay for title / pause / game over */
    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, BOARD_W, 56);
    lv_obj_set_pos(s_overlay, BOARD_X, BOARD_Y + (BOARD_H - 56) / 2);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_t *ot = ui_label(s_overlay, &sol_font_20, COL_GREEN, "");
    lv_obj_set_width(ot, BOARD_W);
    lv_obj_set_style_text_align(ot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *os = ui_label(s_overlay, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_width(os, BOARD_W);
    lv_obj_set_style_text_align(os, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(os, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_hint = ui_hint(root, "");
    go_title();
}

static void leave(void)
{
    best_save();
    s_root = s_board_bg = s_hint = NULL;
    s_score = s_level = s_lines = s_best_lab = s_status = NULL;
    s_overlay = NULL;
    memset(s_cell, 0, sizeof(s_cell));
    memset(s_next_cell, 0, sizeof(s_next_cell));
}

static void on_tick(void)
{
    if (s_state != ST_PLAY) {
        return;
    }
    if (--s_fall > 0) {
        return;
    }
    s_fall = fall_ticks();
    gravity_tick();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (s_state == ST_TITLE) {
        if (k == OS_KEY_SELECT && !repeat) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            return false;   /* exit app */
        }
        return true;
    }

    if (s_state == ST_OVER) {
        if (k == OS_KEY_SELECT && !repeat) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            go_title();
            return true;
        }
        return true;
    }

    if (s_state == ST_PAUSE) {
        if (k == OS_KEY_SELECT && !repeat) {
            go_play();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            best_save();
            go_title();
            return true;
        }
        return true;
    }

    /* ST_PLAY */
    switch (k) {
    case OS_KEY_LEFT:
        try_move(-1, 0);
        return true;
    case OS_KEY_RIGHT:
        try_move(1, 0);
        return true;
    case OS_KEY_DOWN:
        soft_drop_step();
        return true;
    case OS_KEY_UP:
        if (!repeat) {
            try_rotate();
        }
        return true;
    case OS_KEY_SELECT:
        if (!repeat) {
            try_rotate();
        }
        return true;
    case OS_KEY_BACK:
        if (!repeat) {
            go_pause();
        }
        return true;
    default:
        return false;
    }
}

const os_app_t app_tetris = {
    .id = "tetris",
    .name = "Tetris",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
