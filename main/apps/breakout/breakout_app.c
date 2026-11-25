// Breakout: paddle, ball, bricks, lives, and score for the BP26 badge
// (320×240). LEFT/RIGHT move the paddle; SELECT launches / starts /
// resumes; CANCEL pauses or exits. Offline — RGB565 playfield blit only.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "nvs.h"

#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"

#define NVS_NS          "breakout"
#define NVS_BEST        "best"

#define HUD_H           20
#define FIELD_W         320
#define FIELD_H         168

#define BRICK_COLS      10
#define BRICK_ROWS      5
#define BRICK_GAP       2
#define BRICK_W         30
#define BRICK_H         10
#define BRICK_TOP       10
#define BRICK_LEFT      ((FIELD_W - (BRICK_COLS * BRICK_W + (BRICK_COLS - 1) * BRICK_GAP)) / 2)

#define PADDLE_W        48
#define PADDLE_H        6
#define PADDLE_Y        (FIELD_H - 14)
#define PADDLE_SPEED    10

#define BALL_S          5
#define BALL_SPEED0     3
#define BALL_SPEED_MAX  6
#define LIVES_START     3

/* Arcade night palette — Solana-adjacent accents, original layout */
#define PX_FIELD        0x0A1020
#define PX_WALL         0x2A3858
#define PX_PADDLE       0xF0E8D0
#define PX_BALL         0xFFFFFF
#define PX_LIFE         0xFF754A

#define UI_TITLE        0x80ECFF
#define UI_SCORE        0x14F195
#define UI_BEST         0xB6B0FF
#define UI_BOX_BG       0xF8F0D8
#define UI_BOX_INK      0x203060
#define UI_BOX_EDGE     0x2A3858

static const uint32_t s_row_color[BRICK_ROWS] = {
    0x9945FF,   /* top — purple */
    0xEB54BC,   /* pink */
    0x64A8F2,   /* blue */
    0x80ECFF,   /* aqua */
    0xACFF9B,   /* lime */
};

static const int s_row_points[BRICK_ROWS] = { 50, 40, 30, 20, 10 };

typedef enum {
    ST_IDLE = 0,
    ST_READY,       /* ball glued to paddle, waiting for SELECT */
    ST_PLAY,
    ST_PAUSE,
    ST_OVER,
    ST_CLEAR,       /* level cleared — brief beat before next */
} state_t;

static lv_obj_t *s_root, *s_title, *s_score_lab, *s_best_lab;
static lv_obj_t *s_canvas, *s_overlay, *s_hint;
static lv_image_dsc_t s_dsc;
static uint16_t *s_fb;
static state_t s_state;

static int s_score;
static int32_t s_best;
static int s_lives;
static int s_level;
static int s_clear_ticks;

static bool s_brick[BRICK_ROWS][BRICK_COLS];
static int s_bricks_left;

static int s_paddle_x;          /* left edge */
static int s_ball_x, s_ball_y;  /* top-left of ball */
static int s_vx, s_vy;          /* px per tick */
static bool s_stuck;            /* ball rides paddle until launch */

// ---- colour / blit --------------------------------------------------------

static uint16_t color_u16(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t c)
{
    if (!s_fb || w <= 0 || h <= 0) {
        return;
    }
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= FIELD_H) {
            continue;
        }
        uint16_t *row = &s_fb[y * FIELD_W];
        for (int x = x0; x < x0 + w; x++) {
            if (x >= 0 && x < FIELD_W) {
                row[x] = c;
            }
        }
    }
}

static void brick_rect(int row, int col, int *x, int *y, int *w, int *h)
{
    *x = BRICK_LEFT + col * (BRICK_W + BRICK_GAP);
    *y = BRICK_TOP + row * (BRICK_H + BRICK_GAP);
    *w = BRICK_W;
    *h = BRICK_H;
}

// ---- NVS ------------------------------------------------------------------

static void prefs_load(void)
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

// ---- HUD / overlay --------------------------------------------------------

static void refresh_hud(void)
{
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

    fill_rect(0, 0, FIELD_W, FIELD_H, color_u16(PX_FIELD));
    /* thin side walls */
    fill_rect(0, 0, 2, FIELD_H, color_u16(PX_WALL));
    fill_rect(FIELD_W - 2, 0, 2, FIELD_H, color_u16(PX_WALL));
    fill_rect(0, 0, FIELD_W, 2, color_u16(PX_WALL));

    for (int r = 0; r < BRICK_ROWS; r++) {
        uint16_t c = color_u16(s_row_color[r]);
        for (int col = 0; col < BRICK_COLS; col++) {
            if (!s_brick[r][col]) {
                continue;
            }
            int x, y, w, h;
            brick_rect(r, col, &x, &y, &w, &h);
            fill_rect(x, y, w, h, c);
            /* highlight lip */
            fill_rect(x, y, w, 1, color_u16(0xFFFFFF));
        }
    }

    fill_rect(s_paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, color_u16(PX_PADDLE));
    fill_rect(s_ball_x, s_ball_y, BALL_S, BALL_S, color_u16(PX_BALL));

    /* life pips above paddle */
    uint16_t life = color_u16(PX_LIFE);
    for (int i = 0; i < s_lives && i < 5; i++) {
        fill_rect(6 + i * 10, FIELD_H - 8, 6, 3, life);
    }

    if (s_canvas) {
        lv_obj_invalidate(s_canvas);
    }
}

// ---- game -----------------------------------------------------------------

static int ball_speed(void)
{
    int sp = BALL_SPEED0 + (s_level - 1);
    if (sp > BALL_SPEED_MAX) {
        sp = BALL_SPEED_MAX;
    }
    return sp;
}

static void reset_bricks(void)
{
    s_bricks_left = 0;
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            s_brick[r][c] = true;
            s_bricks_left++;
        }
    }
}

static void glue_ball(void)
{
    s_stuck = true;
    s_ball_x = s_paddle_x + PADDLE_W / 2 - BALL_S / 2;
    s_ball_y = PADDLE_Y - BALL_S - 1;
    s_vx = 0;
    s_vy = 0;
}

static void launch_ball(void)
{
    if (!s_stuck) {
        return;
    }
    s_stuck = false;
    int sp = ball_speed();
    /* random left/right; always up */
    s_vx = (esp_random() & 1) ? sp : -sp;
    s_vy = -sp;
}

static void clamp_paddle(void)
{
    if (s_paddle_x < 2) {
        s_paddle_x = 2;
    }
    if (s_paddle_x + PADDLE_W > FIELD_W - 2) {
        s_paddle_x = FIELD_W - 2 - PADDLE_W;
    }
    if (s_stuck) {
        s_ball_x = s_paddle_x + PADDLE_W / 2 - BALL_S / 2;
        s_ball_y = PADDLE_Y - BALL_S - 1;
    }
}

static void move_paddle(int dx)
{
    s_paddle_x += dx;
    clamp_paddle();
    redraw();
}

static void go_ready(void)
{
    s_state = ST_READY;
    glue_ball();
    overlay_show(false, NULL, NULL);
    set_hint("SELECT launch  L/R paddle  CANCEL pause");
    refresh_hud();
    redraw();
}

static void idle_overlay(void)
{
    overlay_show(true, "BREAKOUT", "SELECT start");
    set_hint("L/R paddle  SELECT start  CANCEL exit");
}

static void go_idle(void)
{
    s_state = ST_IDLE;
    s_score = 0;
    s_lives = LIVES_START;
    s_level = 1;
    s_clear_ticks = 0;
    s_paddle_x = (FIELD_W - PADDLE_W) / 2;
    reset_bricks();
    glue_ball();
    refresh_hud();
    idle_overlay();
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
    if (s_stuck) {
        go_ready();
        return;
    }
    s_state = ST_PLAY;
    overlay_show(false, NULL, NULL);
    set_hint("L/R paddle  CANCEL pause");
    redraw();
}

static void game_over(void)
{
    s_state = ST_OVER;
    bool is_new = s_score > s_best;
    best_save();
    refresh_hud();
    char sub[48];
    if (is_new) {
        snprintf(sub, sizeof(sub), "NEW BEST %d — SELECT", s_score);
    } else {
        snprintf(sub, sizeof(sub), "score %d — SELECT again", s_score);
    }
    overlay_show(true, "GAME OVER", sub);
    set_hint("SELECT again  CANCEL exit");
    os_toast("Game over — %d", s_score);
    redraw();
}

static void lose_life(void)
{
    s_lives--;
    refresh_hud();
    if (s_lives <= 0) {
        game_over();
        return;
    }
    go_ready();
    os_toast("Lives %d", s_lives);
}

static void level_clear(void)
{
    s_state = ST_CLEAR;
    s_clear_ticks = 8;      /* ~0.8 s */
    overlay_show(true, "CLEAR!", "next level…");
    set_hint("");
    redraw();
}

static void next_level(void)
{
    s_level++;
    reset_bricks();
    s_paddle_x = (FIELD_W - PADDLE_W) / 2;
    go_ready();
    os_toast("Level %d", s_level);
}

static void start_game(void)
{
    s_score = 0;
    s_lives = LIVES_START;
    s_level = 1;
    s_clear_ticks = 0;
    s_paddle_x = (FIELD_W - PADDLE_W) / 2;
    reset_bricks();
    refresh_hud();
    go_ready();
}

static void bounce_paddle(void)
{
    /* hit zone → angle: left third hard left, mid slight, right hard right */
    int mid = s_paddle_x + PADDLE_W / 2;
    int bx = s_ball_x + BALL_S / 2;
    int sp = ball_speed();
    int zone = bx - mid;
    if (zone < -PADDLE_W / 6) {
        s_vx = -sp;
    } else if (zone > PADDLE_W / 6) {
        s_vx = sp;
    } else {
        /* keep horizontal sign; ensure non-zero */
        if (s_vx == 0) {
            s_vx = (esp_random() & 1) ? sp : -sp;
        } else {
            s_vx = (s_vx > 0) ? sp : -sp;
        }
    }
    s_vy = -sp;
    /* sit just above paddle to avoid re-hit */
    s_ball_y = PADDLE_Y - BALL_S - 1;
}

static bool hit_brick_at(int cx, int cy)
{
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!s_brick[r][c]) {
                continue;
            }
            int x, y, w, h;
            brick_rect(r, c, &x, &y, &w, &h);
            if (cx >= x && cx < x + w && cy >= y && cy < y + h) {
                s_brick[r][c] = false;
                s_bricks_left--;
                s_score += s_row_points[r];
                if (s_score > s_best) {
                    /* live best update while playing */
                    best_save();
                }
                refresh_hud();
                return true;
            }
        }
    }
    return false;
}

static void resolve_brick_hits(void)
{
    /* sample ball corners + centre against bricks; bounce on first hit */
    int pts[5][2] = {
        { s_ball_x, s_ball_y },
        { s_ball_x + BALL_S - 1, s_ball_y },
        { s_ball_x, s_ball_y + BALL_S - 1 },
        { s_ball_x + BALL_S - 1, s_ball_y + BALL_S - 1 },
        { s_ball_x + BALL_S / 2, s_ball_y + BALL_S / 2 },
    };
    for (int i = 0; i < 5; i++) {
        if (hit_brick_at(pts[i][0], pts[i][1])) {
            /* prefer vertical bounce; flip X if hitting near side */
            int bx = pts[i][0];
            int by = pts[i][1];
            /* rough side vs top/bottom: compare penetration direction */
            (void)bx;
            (void)by;
            s_vy = -s_vy;
            if (s_bricks_left <= 0) {
                level_clear();
            }
            return;
        }
    }
}

static void step(void)
{
    if (s_stuck) {
        return;
    }

    s_ball_x += s_vx;
    s_ball_y += s_vy;

    /* walls */
    if (s_ball_x < 2) {
        s_ball_x = 2;
        s_vx = -s_vx;
    } else if (s_ball_x + BALL_S > FIELD_W - 2) {
        s_ball_x = FIELD_W - 2 - BALL_S;
        s_vx = -s_vx;
    }
    if (s_ball_y < 2) {
        s_ball_y = 2;
        s_vy = -s_vy;
    }

    /* fallen off bottom */
    if (s_ball_y > FIELD_H) {
        lose_life();
        return;
    }

    /* paddle */
    if (s_vy > 0 &&
        s_ball_y + BALL_S >= PADDLE_Y &&
        s_ball_y < PADDLE_Y + PADDLE_H &&
        s_ball_x + BALL_S > s_paddle_x &&
        s_ball_x < s_paddle_x + PADDLE_W) {
        bounce_paddle();
    }

    resolve_brick_hits();
    if (s_state != ST_PLAY) {
        return;
    }

    redraw();
}

// ---- lifecycle ------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_state = ST_IDLE;
    s_score = 0;
    s_lives = LIVES_START;
    s_level = 1;
    s_fb = NULL;
    s_canvas = NULL;
    s_overlay = NULL;

    prefs_load();

    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(PX_FIELD), 0);

    s_title = ui_label(root, &sol_font_20, UI_TITLE, "Breakout");
    lv_obj_set_pos(s_title, 8, 1);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_TITLE), 0);

    s_score_lab = ui_label(root, &sol_mono_12, UI_SCORE, "");
    lv_obj_set_pos(s_score_lab, 118, 5);
    lv_obj_set_style_text_color(s_score_lab, lv_color_hex(UI_SCORE), 0);

    s_best_lab = ui_label(root, &sol_mono_12, UI_BEST, "");
    lv_obj_set_pos(s_best_lab, 220, 5);
    lv_obj_set_style_text_color(s_best_lab, lv_color_hex(UI_BEST), 0);
    refresh_hud();

    size_t bytes = (size_t)FIELD_W * FIELD_H * 2;
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
    s_dsc.header.w = FIELD_W;
    s_dsc.header.h = FIELD_H;
    s_dsc.header.stride = FIELD_W * 2;
    s_dsc.data_size = (uint32_t)bytes;
    s_dsc.data = (const uint8_t *)s_fb;

    int gy = HUD_H;

    s_canvas = lv_image_create(root);
    lv_image_set_src(s_canvas, &s_dsc);
    lv_obj_set_pos(s_canvas, 0, gy);

    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, FIELD_W - 48, 52);
    lv_obj_set_pos(s_overlay, 24, gy + (FIELD_H - 52) / 2);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(UI_BOX_BG), 0);
    lv_obj_set_style_border_width(s_overlay, 3, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(UI_BOX_EDGE), 0);
    lv_obj_set_style_radius(s_overlay, 6, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_t *ot = ui_label(s_overlay, &sol_font_20, UI_BOX_INK, "");
    lv_obj_set_width(ot, FIELD_W - 48);
    lv_obj_set_style_text_align(ot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ot, lv_color_hex(UI_BOX_INK), 0);
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *os = ui_label(s_overlay, &sol_mono_12, UI_BEST, "");
    lv_obj_set_width(os, FIELD_W - 48);
    lv_obj_set_style_text_align(os, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(os, lv_color_hex(UI_BEST), 0);
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
    s_canvas = NULL;
    s_overlay = NULL;
}

static void on_tick(void)
{
    if (s_state == ST_CLEAR) {
        if (s_clear_ticks > 0) {
            s_clear_ticks--;
        }
        if (s_clear_ticks <= 0) {
            next_level();
        }
        return;
    }
    if (s_state != ST_PLAY) {
        return;
    }
    step();
}

static bool on_key(os_key_t k, bool repeat)
{
    /* paddle moves accept repeats while playing / ready */
    if ((s_state == ST_PLAY || s_state == ST_READY) &&
        (k == OS_KEY_LEFT || k == OS_KEY_RIGHT)) {
        move_paddle(k == OS_KEY_LEFT ? -PADDLE_SPEED : PADDLE_SPEED);
        return true;
    }

    if (repeat) {
        return true;
    }

    if (s_state == ST_IDLE) {
        if (k == OS_KEY_SELECT) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK) {
            return false;
        }
        return true;
    }

    if (s_state == ST_OVER) {
        if (k == OS_KEY_SELECT) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK) {
            return false;
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

    if (s_state == ST_CLEAR) {
        return true;
    }

    if (s_state == ST_READY) {
        if (k == OS_KEY_SELECT) {
            launch_ball();
            s_state = ST_PLAY;
            set_hint("L/R paddle  CANCEL pause");
            redraw();
            return true;
        }
        if (k == OS_KEY_BACK) {
            go_pause();
            return true;
        }
        return true;
    }

    if (s_state == ST_PLAY) {
        if (k == OS_KEY_BACK) {
            go_pause();
            return true;
        }
        if (k == OS_KEY_SELECT && s_stuck) {
            launch_ball();
            return true;
        }
        return true;
    }

    return false;
}

const os_app_t app_breakout = {
    .id = "breakout",
    .name = "Breakout",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
