// Classic Tetris for the BP26 badge (320×240, Solana OS).
// Seven tetrominoes, rotate, soft drop, line clear, score / level speed-up.
// Offline — no Wi-Fi, mesh, or SD. Best score kept in NVS.
// Visual theme: handheld-RPG battle-menu palette (original colours; no
// copyrighted creature sprites or trade names in art or strings).

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
#define BOARD_X         6
#define BOARD_Y         0

#define PIECE_N         7
#define BAG_N           7

/* Right-hand HUD column (fits OS_ROOT_W = 320). */
#define HUD_X           (BOARD_X + BOARD_W + 12)
#define HUD_W           (OS_ROOT_W - HUD_X - 6)
#define NEXT_CELL       11
#define NEXT_GAP        12
#define NEXT_BOX        (4 * NEXT_GAP)

/* Pocket-trainer field palette — original hues inspired by classic handheld
 * RPG menus (cream cards, ink borders, grass well). Not brand IP. */
#define TET_CREAM       0xF8F0D8
#define TET_CREAM_HI    0xFFF8E8
#define TET_INK         0x1A1A20
#define TET_MUTED       0x6A6458
#define TET_WELL        0x1E3A32
#define TET_CELL_EMPTY  0x2A4A40
#define TET_CELL_LINE   0x3A5C50
#define TET_GOLD        0xE8C840
#define TET_RED         0xD04040
#define TET_LEAF        0x38A050
#define TET_SKY         0x58C8E8
#define TET_OCEAN       0x4070D0
#define TET_FLAME       0xE05050
#define TET_SUN         0xF0D030
#define TET_ORANGE      0xE88830
#define TET_LEAF2       0x50B848
#define TET_PSY         0xA060C8

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

// I J L O S T Z — Guideline roles with bright type-inspired fills.
static const piece_def_t k_pieces[PIECE_N] = {
    { /* I — sky cyan */
        {
            {{0, 1}, {1, 1}, {2, 1}, {3, 1}},
            {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
            {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
            {{1, 0}, {1, 1}, {1, 2}, {1, 3}},
        },
        TET_SKY,
    },
    { /* J — ocean blue */
        {
            {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
            {{1, 0}, {1, 1}, {0, 2}, {1, 2}},
        },
        TET_OCEAN,
    },
    { /* L — warm orange */
        {
            {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
            {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
        },
        TET_ORANGE,
    },
    { /* O — sun yellow */
        {
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        },
        TET_SUN,
    },
    { /* S — leaf green */
        {
            {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
            {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
            {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
            {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
        },
        TET_LEAF2,
    },
    { /* T — psychedelic purple */
        {
            {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
            {{1, 0}, {0, 1}, {1, 1}, {1, 2}},
        },
        TET_PSY,
    },
    { /* Z — flame red */
        {
            {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
            {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
            {{1, 0}, {0, 1}, {1, 1}, {0, 2}},
        },
        TET_FLAME,
    },
};

static lv_obj_t *s_root, *s_board_bg, *s_hint;
static lv_obj_t *s_cell[ROWS][COLS];
static lv_obj_t *s_score_val, *s_level_val, *s_lines_val, *s_best_val;
static lv_obj_t *s_next_cell[4][4];
static lv_obj_t *s_overlay, *s_ov_title, *s_ov_sub, *s_ov_score;

static state_t s_state;
static uint8_t s_grid[ROWS][COLS];          /* 0 empty, 1..7 piece id */
static int s_px, s_py, s_prot, s_pid;       /* active piece */
static int s_next;
static int s_bag[BAG_N], s_bag_i;
static int32_t s_score_v, s_best;
static int s_level_v, s_lines_v;
static int s_fall;                          /* ticks until gravity */
static bool s_new_best;                     /* set on game over when best beaten */

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

static int ghost_y(void)
{
    int y = s_py;
    while (fits(s_pid, s_prot, s_px, y + 1)) {
        y++;
    }
    return y;
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
        return TET_CELL_EMPTY;
    }
    return k_pieces[id - 1].color;
}

/* Darken a 24-bit RGB colour for cell bevel / ghost outline. */
static uint32_t shade_rgb(uint32_t c, int num, int den)
{
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)num / (uint32_t)den;
    uint32_t g = ((c >> 8) & 0xFF) * (uint32_t)num / (uint32_t)den;
    uint32_t b = (c & 0xFF) * (uint32_t)num / (uint32_t)den;
    return (r << 16) | (g << 8) | b;
}

typedef enum {
    CELL_EMPTY = 0,
    CELL_SOLID,
    CELL_GHOST,
} cell_look_t;

static void paint_cell(lv_obj_t *obj, uint32_t c, cell_look_t look)
{
    if (!obj) {
        return;
    }
    if (look == CELL_EMPTY) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(TET_CELL_EMPTY), 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(TET_CELL_LINE), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_radius(obj, 1, 0);
    } else if (look == CELL_GHOST) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(TET_CELL_EMPTY), 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(shade_rgb(c, 3, 4)), 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_radius(obj, 1, 0);
    } else {
        lv_obj_set_style_bg_color(obj, lv_color_hex(c), 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(shade_rgb(c, 1, 2)), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_radius(obj, 2, 0);
    }
}

static void paint_board(void)
{
    uint8_t view[ROWS][COLS];
    uint8_t ghost[ROWS][COLS];
    memcpy(view, s_grid, sizeof(view));
    memset(ghost, 0, sizeof(ghost));

    if (s_state == ST_PLAY || s_state == ST_PAUSE) {
        const piece_def_t *p = &k_pieces[s_pid];
        int gy = ghost_y();
        for (int i = 0; i < 4; i++) {
            int x = s_px + p->rot[s_prot][i].x;
            int y = gy + p->rot[s_prot][i].y;
            if (y >= 0 && y < ROWS && x >= 0 && x < COLS && !view[y][x]) {
                ghost[y][x] = (uint8_t)(s_pid + 1);
            }
        }
        for (int i = 0; i < 4; i++) {
            int x = s_px + p->rot[s_prot][i].x;
            int y = s_py + p->rot[s_prot][i].y;
            if (y >= 0 && y < ROWS && x >= 0 && x < COLS) {
                view[y][x] = (uint8_t)(s_pid + 1);
                ghost[y][x] = 0;
            }
        }
    }

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            uint8_t id = view[y][x];
            if (id) {
                paint_cell(s_cell[y][x], color_for(id), CELL_SOLID);
            } else if (ghost[y][x]) {
                paint_cell(s_cell[y][x], color_for(ghost[y][x]), CELL_GHOST);
            } else {
                paint_cell(s_cell[y][x], TET_CELL_EMPTY, CELL_EMPTY);
            }
        }
    }
}

static void paint_next(void)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            paint_cell(s_next_cell[y][x], TET_CELL_EMPTY, CELL_EMPTY);
        }
    }
    if (s_state == ST_TITLE) {
        return;
    }
    const piece_def_t *p = &k_pieces[s_next];
    int min_x = 3, max_x = 0, min_y = 3, max_y = 0;
    for (int i = 0; i < 4; i++) {
        int x = p->rot[0][i].x;
        int y = p->rot[0][i].y;
        if (x < min_x) {
            min_x = x;
        }
        if (x > max_x) {
            max_x = x;
        }
        if (y < min_y) {
            min_y = y;
        }
        if (y > max_y) {
            max_y = y;
        }
    }
    int ox = (4 - (max_x - min_x + 1)) / 2 - min_x;
    int oy = (4 - (max_y - min_y + 1)) / 2 - min_y;
    for (int i = 0; i < 4; i++) {
        int x = p->rot[0][i].x + ox;
        int y = p->rot[0][i].y + oy;
        if (x >= 0 && x < 4 && y >= 0 && y < 4) {
            paint_cell(s_next_cell[y][x], p->color, CELL_SOLID);
        }
    }
}

static void refresh_hud(void)
{
    if (s_score_val) {
        lv_label_set_text_fmt(s_score_val, "%ld", (long)s_score_v);
    }
    if (s_level_val) {
        lv_label_set_text_fmt(s_level_val, "%d", s_level_v);
    }
    if (s_lines_val) {
        lv_label_set_text_fmt(s_lines_val, "%d", s_lines_v);
    }
    if (s_best_val) {
        lv_label_set_text_fmt(s_best_val, "%ld", (long)s_best);
    }
}

static void set_hint(const char *msg)
{
    if (s_hint) {
        lv_label_set_text(s_hint, msg);
    }
}

static void overlay_show(bool on, const char *title, const char *sub, const char *score_line)
{
    if (!s_overlay) {
        return;
    }
    if (!on) {
        lv_obj_set_hidden(s_overlay, true);
        return;
    }
    lv_obj_set_hidden(s_overlay, false);
    if (s_ov_title) {
        lv_label_set_text(s_ov_title, title ? title : "");
    }
    if (s_ov_sub) {
        lv_label_set_text(s_ov_sub, sub ? sub : "");
    }
    if (s_ov_score) {
        if (score_line && score_line[0]) {
            lv_obj_set_hidden(s_ov_score, false);
            lv_label_set_text(s_ov_score, score_line);
        } else {
            lv_obj_set_hidden(s_ov_score, true);
            lv_label_set_text(s_ov_score, "");
        }
    }
}

// ---- game flow ------------------------------------------------------------

static void game_reset_board(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_score_v = 0;
    s_level_v = 1;
    s_lines_v = 0;
    s_new_best = false;
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
    overlay_show(true, "TETRIS", "SELECT to start", NULL);
    set_hint("SELECT start   CANCEL exit");
}

static void go_play(void)
{
    s_state = ST_PLAY;
    overlay_show(false, NULL, NULL, NULL);
    set_hint("L/R move  UP/SEL rot  DOWN drop");
    paint_board();
    paint_next();
    refresh_hud();
}

static void go_pause(void)
{
    s_state = ST_PAUSE;
    overlay_show(true, "PAUSED", "SELECT resume", NULL);
    set_hint("SELECT resume   CANCEL title");
    paint_board();
}

static void go_over(void)
{
    s_state = ST_OVER;
    if (s_score_v > s_best) {
        s_new_best = true;
    }
    best_save();
    refresh_hud();
    char line[40];
    if (s_new_best) {
        snprintf(line, sizeof(line), "NEW BEST  %ld", (long)s_score_v);
    } else {
        snprintf(line, sizeof(line), "SCORE  %ld", (long)s_score_v);
    }
    overlay_show(true, "GAME OVER", "SELECT play again", line);
    set_hint("SELECT again   CANCEL title");
    os_toast(s_new_best ? "New best — %ld" : "Game over — %ld", (long)s_score_v);
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
    paint_cell(c, TET_CELL_EMPTY, CELL_EMPTY);
    return c;
}

static lv_obj_t *hud_caption(lv_obj_t *parent, int x, int y, int w, const char *text)
{
    lv_obj_t *l = ui_label(parent, &sol_mono_12, TET_MUTED, text);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    return l;
}

static lv_obj_t *hud_value(lv_obj_t *parent, int x, int y, int w,
                           const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = ui_label(parent, font, color, "0");
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    return l;
}

/* Cream status card — thick ink rim like a handheld RPG menu box. */
static lv_obj_t *hud_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = ui_panel(parent, x, y, w, h);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 4, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(TET_CREAM), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(TET_INK), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    return p;
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    best_load();

    /* Soft field wash behind the well (reads as grass, not Solana black). */
    lv_obj_t *field = ui_panel(root, 0, 0, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_style_pad_all(field, 0, 0);
    lv_obj_set_style_radius(field, 0, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x244038), 0);
    lv_obj_set_style_border_width(field, 0, 0);

    /* Playfield well — deep green inset with ink frame. */
    s_board_bg = ui_panel(root, BOARD_X - 3, BOARD_Y, BOARD_W + 6, BOARD_H + 4);
    lv_obj_set_style_pad_all(s_board_bg, 0, 0);
    lv_obj_set_style_radius(s_board_bg, 4, 0);
    lv_obj_set_style_bg_color(s_board_bg, lv_color_hex(TET_WELL), 0);
    lv_obj_set_style_border_color(s_board_bg, lv_color_hex(TET_INK), 0);
    lv_obj_set_style_border_width(s_board_bg, 3, 0);

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            s_cell[y][x] = make_cell(root,
                                     BOARD_X + x * CELL,
                                     BOARD_Y + 2 + y * CELL,
                                     CELL - 1);
        }
    }

    int hx = HUD_X;
    int hw = HUD_W;

    /* Title strip on cream card + gold underline (menu-bar cue). */
    lv_obj_t *title_card = hud_card(root, hx, 0, hw, 24);
    lv_obj_t *title = ui_label(title_card, &sol_font_14, TET_INK, "TETRIS");
    lv_obj_set_pos(title, 6, 3);
    lv_obj_t *gold = ui_panel(root, hx + 2, 22, hw - 4, 3);
    lv_obj_set_style_pad_all(gold, 0, 0);
    lv_obj_set_style_radius(gold, 0, 0);
    lv_obj_set_style_bg_color(gold, lv_color_hex(TET_GOLD), 0);
    lv_obj_set_style_border_width(gold, 0, 0);

    /* SCORE card — bold red value. */
    lv_obj_t *score_card = hud_card(root, hx, 28, hw, 34);
    hud_caption(score_card, 6, 2, hw - 12, "SCORE");
    s_score_val = hud_value(score_card, 6, 13, hw - 12, &sol_font_20, TET_RED);

    /* LV / LINES side-by-side cards. */
    int half = (hw - 4) / 2;
    lv_obj_t *lv_card = hud_card(root, hx, 66, half, 32);
    hud_caption(lv_card, 4, 2, half - 8, "LV");
    s_level_val = hud_value(lv_card, 4, 13, half - 8, &sol_font_14, TET_INK);

    lv_obj_t *ln_card = hud_card(root, hx + half + 4, 66, half, 32);
    hud_caption(ln_card, 4, 2, half - 8, "LINES");
    s_lines_val = hud_value(ln_card, 4, 13, half - 8, &sol_font_14, TET_INK);

    /* BEST + NEXT — must clear hint bar (y + h ≤ OS_ROOT_H - UI_HINT_H ≈ 202). */
    lv_obj_t *best_card = hud_card(root, hx, 102, hw, 26);
    hud_caption(best_card, 6, 1, hw - 12, "BEST");
    s_best_val = hud_value(best_card, 6, 11, hw - 12, &sol_mono_12, TET_LEAF);

    lv_obj_t *next_card = hud_card(root, hx, 132, hw, NEXT_BOX + 16);
    hud_caption(next_card, 6, 1, hw - 12, "NEXT");

    int nx = hx + (hw - NEXT_BOX) / 2;
    int ny = 146;
    lv_obj_t *next_bg = ui_panel(root, nx - 2, ny - 2, NEXT_BOX + 4, NEXT_BOX + 4);
    lv_obj_set_style_pad_all(next_bg, 0, 0);
    lv_obj_set_style_radius(next_bg, 3, 0);
    lv_obj_set_style_bg_color(next_bg, lv_color_hex(TET_WELL), 0);
    lv_obj_set_style_border_color(next_bg, lv_color_hex(TET_INK), 0);
    lv_obj_set_style_border_width(next_bg, 2, 0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            s_next_cell[y][x] = make_cell(root, nx + x * NEXT_GAP, ny + y * NEXT_GAP, NEXT_CELL);
        }
    }

    /* Overlay: cream dialogue card on the playfield. */
    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, BOARD_W - 4, 78);
    lv_obj_set_pos(s_overlay, BOARD_X + 2, BOARD_Y + 2 + (BOARD_H - 78) / 2);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(TET_CREAM_HI), 0);
    lv_obj_set_style_border_width(s_overlay, 3, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(TET_INK), 0);
    lv_obj_set_style_radius(s_overlay, 6, 0);
    lv_obj_set_scrollable(s_overlay, false);

    s_ov_title = ui_label(s_overlay, &sol_font_20, TET_RED, "");
    lv_obj_set_width(s_ov_title, BOARD_W - 12);
    lv_obj_set_style_text_align(s_ov_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ov_title, LV_ALIGN_TOP_MID, 0, 8);

    s_ov_score = ui_label(s_overlay, &sol_mono_12, TET_LEAF, "");
    lv_obj_set_width(s_ov_score, BOARD_W - 12);
    lv_obj_set_style_text_align(s_ov_score, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ov_score, LV_ALIGN_CENTER, 0, 4);

    s_ov_sub = ui_label(s_overlay, &sol_mono_12, TET_INK, "");
    lv_obj_set_width(s_ov_sub, BOARD_W - 12);
    lv_obj_set_style_text_align(s_ov_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ov_sub, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_hint = ui_hint(root, "");
    go_title();
}

static void leave(void)
{
    best_save();
    s_root = s_board_bg = s_hint = NULL;
    s_score_val = s_level_val = s_lines_val = s_best_val = NULL;
    s_overlay = s_ov_title = s_ov_sub = s_ov_score = NULL;
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
    .accent = TET_GOLD,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
