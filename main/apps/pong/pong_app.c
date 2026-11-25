// Pong: 1P vs AI or local 2P for the BP26 badge.
// P1: UP/DOWN. 2P: LEFT=up / RIGHT=down for the right paddle.
// SELECT start·resume·restart; CANCEL pause / title / exit. Offline; NVS best.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "nvs.h"

#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"

#define NVS_NS          "pong"
#define NVS_BEST        "best"
#define NVS_MODE        "mode"

#define HUD_H           28
#define FRAME_PAD       3
#define COURT_W         300
#define COURT_H         160

#define PADDLE_W        5
#define PADDLE_H        36
#define PADDLE_MARGIN   8
#define BALL_S          6

#define WIN_SCORE       7
#define PADDLE_STEP     10

/* Court colours — original arcade look (no franchise art). */
#define PX_COURT        0x0B1A3A
#define PX_LINE         0x3A5080
#define PX_P1           0x2EC4B6
#define PX_P2           0xFF9F1C
#define PX_BALL         0xFFF8E0
#define PX_NET          0x5EC8F0

#define UI_TITLE        0x5EC8F0
#define UI_SCORE        0xFFD60A
#define UI_BEST         0x3A86FF
#define UI_BOX_BG       0xFFF8E0
#define UI_BOX_EDGE     0x1A2A6C
#define UI_BOX_INK      0x1A1A2E

typedef enum {
    ST_IDLE = 0,
    ST_PLAY,
    ST_PAUSE,
    ST_OVER,
} state_t;

typedef enum {
    MODE_AI_EASY = 0,
    MODE_AI_NORMAL,
    MODE_AI_HARD,
    MODE_2P,
    MODE_N,
} mode_t;

static lv_obj_t *s_root, *s_title, *s_score_lab, *s_best_lab, *s_hint;
static lv_obj_t *s_frame, *s_canvas, *s_overlay;
static lv_image_dsc_t s_dsc;
static uint16_t *s_fb;

static state_t s_state;
static mode_t s_mode;
static int s_score_l, s_score_r;
static int32_t s_best;              /* most points scored in a finished match (sum) */

static int s_p1_y, s_p2_y;          /* paddle top */
static int s_ball_x, s_ball_y;
static int s_vx, s_vy;
static int s_serve_wait;            /* ticks before ball moves after serve */
static bool s_serve_to_right;

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
        if (y < 0 || y >= COURT_H) {
            continue;
        }
        uint16_t *row = &s_fb[y * COURT_W];
        for (int x = x0; x < x0 + w; x++) {
            if (x >= 0 && x < COURT_W) {
                row[x] = c;
            }
        }
    }
}

static const char *mode_name(mode_t m)
{
    switch (m) {
    case MODE_AI_EASY:
        return "1P Easy";
    case MODE_AI_NORMAL:
        return "1P Normal";
    case MODE_AI_HARD:
        return "1P Hard";
    case MODE_2P:
        return "2 Players";
    default:
        return "?";
    }
}

static int ai_max_step(void)
{
    switch (s_mode) {
    case MODE_AI_EASY:
        return 4;
    case MODE_AI_HARD:
        return 12;
    default:
        return 8;
    }
}

// ---- NVS ------------------------------------------------------------------

static void prefs_load(void)
{
    s_best = 0;
    s_mode = MODE_AI_NORMAL;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, NVS_BEST, &v) == ESP_OK && v > 0) {
        s_best = v;
    }
    if (nvs_get_i32(h, NVS_MODE, &v) == ESP_OK && v >= 0 && v < MODE_N) {
        s_mode = (mode_t)v;
    }
    nvs_close(h);
}

static void best_save(void)
{
    int total = s_score_l + s_score_r;
    if (total <= s_best) {
        return;
    }
    s_best = total;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_BEST, s_best);
    nvs_commit(h);
    nvs_close(h);
}

static void mode_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_MODE, (int32_t)s_mode);
    nvs_commit(h);
    nvs_close(h);
}

// ---- HUD / overlay --------------------------------------------------------

static void refresh_hud(void)
{
    if (s_title) {
        lv_label_set_text_fmt(s_title, "Pong · %s", mode_name(s_mode));
    }
    if (s_score_lab) {
        lv_label_set_text_fmt(s_score_lab, "%d  :  %d", s_score_l, s_score_r);
    }
    if (s_best_lab) {
        lv_label_set_text_fmt(s_best_lab, "BEST %ld", (long)s_best);
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

    uint16_t court = color_u16(PX_COURT);
    uint16_t line = color_u16(PX_LINE);
    uint16_t net = color_u16(PX_NET);
    uint16_t p1 = color_u16(PX_P1);
    uint16_t p2 = color_u16(PX_P2);
    uint16_t ball = color_u16(PX_BALL);

    for (int i = 0; i < COURT_W * COURT_H; i++) {
        s_fb[i] = court;
    }

    /* top / bottom rails */
    fill_rect(0, 0, COURT_W, 2, line);
    fill_rect(0, COURT_H - 2, COURT_W, 2, line);

    /* dashed center net */
    int cx = COURT_W / 2;
    for (int y = 4; y < COURT_H - 4; y += 10) {
        fill_rect(cx - 1, y, 2, 6, net);
    }

    fill_rect(PADDLE_MARGIN, s_p1_y, PADDLE_W, PADDLE_H, p1);
    fill_rect(COURT_W - PADDLE_MARGIN - PADDLE_W, s_p2_y, PADDLE_W, PADDLE_H, p2);

    if (s_serve_wait <= 0 || s_state == ST_IDLE) {
        fill_rect(s_ball_x, s_ball_y, BALL_S, BALL_S, ball);
    }

    if (s_canvas) {
        lv_obj_invalidate(s_canvas);
    }
}

// ---- game -----------------------------------------------------------------

static void clamp_paddle(int *y)
{
    if (*y < 2) {
        *y = 2;
    }
    if (*y > COURT_H - PADDLE_H - 2) {
        *y = COURT_H - PADDLE_H - 2;
    }
}

static void move_paddle(int *y, int dy)
{
    *y += dy;
    clamp_paddle(y);
}

static void serve_ball(void)
{
    s_ball_x = COURT_W / 2 - BALL_S / 2;
    s_ball_y = COURT_H / 2 - BALL_S / 2;
    int speed = (s_mode == MODE_AI_HARD) ? 9 : (s_mode == MODE_AI_EASY) ? 6 : 7;
    s_vx = s_serve_to_right ? speed : -speed;
    /* slight vertical bias so rallies aren't flat */
    int vy = (int)(esp_random() % 5) - 2;
    if (vy == 0) {
        vy = (esp_random() & 1) ? 1 : -1;
    }
    s_vy = vy;
    s_serve_wait = 4;   /* ~400 ms */
}

static void idle_overlay(void)
{
    char sub[48];
    snprintf(sub, sizeof(sub), "%s — SELECT start", mode_name(s_mode));
    overlay_show(true, "PONG", sub);
    if (s_mode == MODE_2P) {
        set_hint("L/R mode  SELECT start  CANCEL exit");
    } else {
        set_hint("L/R mode  SELECT start  CANCEL exit");
    }
}

static void go_idle(void)
{
    s_state = ST_IDLE;
    s_score_l = 0;
    s_score_r = 0;
    s_p1_y = (COURT_H - PADDLE_H) / 2;
    s_p2_y = (COURT_H - PADDLE_H) / 2;
    s_ball_x = COURT_W / 2 - BALL_S / 2;
    s_ball_y = COURT_H / 2 - BALL_S / 2;
    s_vx = s_vy = 0;
    s_serve_wait = 0;
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
    s_state = ST_PLAY;
    overlay_show(false, NULL, NULL);
    if (s_mode == MODE_2P) {
        set_hint("P1 U/D  P2 L/R  CANCEL pause");
    } else {
        set_hint("UP/DOWN paddle  CANCEL pause");
    }
    redraw();
}

static void match_over(bool left_won)
{
    s_state = ST_OVER;
    best_save();
    refresh_hud();
    char sub[48];
    if (s_mode == MODE_2P) {
        snprintf(sub, sizeof(sub), "P%d wins %d-%d — SELECT",
                 left_won ? 1 : 2, s_score_l, s_score_r);
    } else {
        snprintf(sub, sizeof(sub), "%s %d-%d — SELECT",
                 left_won ? "You win" : "AI wins", s_score_l, s_score_r);
    }
    overlay_show(true, "GAME OVER", sub);
    set_hint("SELECT again  CANCEL title");
    os_toast("%s", left_won ? (s_mode == MODE_2P ? "P1 wins" : "You win")
                            : (s_mode == MODE_2P ? "P2 wins" : "AI wins"));
    redraw();
}

static void start_game(void)
{
    s_score_l = 0;
    s_score_r = 0;
    s_p1_y = (COURT_H - PADDLE_H) / 2;
    s_p2_y = (COURT_H - PADDLE_H) / 2;
    s_serve_to_right = true;
    s_state = ST_PLAY;
    serve_ball();
    refresh_hud();
    overlay_show(false, NULL, NULL);
    if (s_mode == MODE_2P) {
        set_hint("P1 U/D  P2 L/R  CANCEL pause");
    } else {
        set_hint("UP/DOWN paddle  CANCEL pause");
    }
    redraw();
}

static void cycle_mode(int dir)
{
    int m = (int)s_mode + dir;
    if (m < 0) {
        m = MODE_N - 1;
    } else if (m >= MODE_N) {
        m = 0;
    }
    s_mode = (mode_t)m;
    mode_save();
    idle_overlay();
    refresh_hud();
}

static void ai_tick(void)
{
    if (s_mode == MODE_2P) {
        return;
    }
    /* chase ball center; lag more on Easy */
    int target = s_ball_y + BALL_S / 2 - PADDLE_H / 2;
    if (s_mode == MODE_AI_EASY && (esp_random() % 4) == 0) {
        return; /* occasional idle frame */
    }
    int dy = target - s_p2_y;
    int max = ai_max_step();
    if (dy > max) {
        dy = max;
    } else if (dy < -max) {
        dy = -max;
    }
    move_paddle(&s_p2_y, dy);
}

static void bounce_paddle(int paddle_y, int dir)
{
    /* reverse horizontal; spin from hit offset */
    s_vx = (s_vx < 0 ? -s_vx : s_vx);
    if (dir < 0) {
        s_vx = -s_vx;
    }
    int hit = (s_ball_y + BALL_S / 2) - (paddle_y + PADDLE_H / 2);
    s_vy = hit / 6;
    if (s_vy == 0) {
        s_vy = (esp_random() & 1) ? 1 : -1;
    }
    if (s_vy > 6) {
        s_vy = 6;
    }
    if (s_vy < -6) {
        s_vy = -6;
    }
    /* slight speed-up on bounce, cap */
    if (s_vx > 0 && s_vx < 14) {
        s_vx++;
    } else if (s_vx < 0 && s_vx > -14) {
        s_vx--;
    }
}

static void score_point(bool left_scored)
{
    if (left_scored) {
        s_score_l++;
    } else {
        s_score_r++;
    }
    refresh_hud();
    if (s_score_l >= WIN_SCORE || s_score_r >= WIN_SCORE) {
        match_over(s_score_l > s_score_r);
        return;
    }
    s_serve_to_right = !left_scored;
    serve_ball();
    redraw();
}

static void physics_step(void)
{
    if (s_serve_wait > 0) {
        s_serve_wait--;
        ai_tick();
        redraw();
        return;
    }

    s_ball_x += s_vx;
    s_ball_y += s_vy;

    /* top / bottom bounce */
    if (s_ball_y < 2) {
        s_ball_y = 2;
        s_vy = -s_vy;
    } else if (s_ball_y + BALL_S > COURT_H - 2) {
        s_ball_y = COURT_H - 2 - BALL_S;
        s_vy = -s_vy;
    }

    int p1_x = PADDLE_MARGIN;
    int p2_x = COURT_W - PADDLE_MARGIN - PADDLE_W;

    /* left paddle */
    if (s_vx < 0 &&
        s_ball_x <= p1_x + PADDLE_W &&
        s_ball_x + BALL_S >= p1_x &&
        s_ball_y + BALL_S >= s_p1_y &&
        s_ball_y <= s_p1_y + PADDLE_H) {
        s_ball_x = p1_x + PADDLE_W;
        bounce_paddle(s_p1_y, +1);
    }

    /* right paddle */
    if (s_vx > 0 &&
        s_ball_x + BALL_S >= p2_x &&
        s_ball_x <= p2_x + PADDLE_W &&
        s_ball_y + BALL_S >= s_p2_y &&
        s_ball_y <= s_p2_y + PADDLE_H) {
        s_ball_x = p2_x - BALL_S;
        bounce_paddle(s_p2_y, -1);
    }

    ai_tick();

    if (s_ball_x + BALL_S < 0) {
        score_point(false);     /* right scores */
        return;
    }
    if (s_ball_x > COURT_W) {
        score_point(true);      /* left scores */
        return;
    }

    redraw();
}

// ---- lifecycle ------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_state = ST_IDLE;
    s_score_l = s_score_r = 0;
    s_fb = NULL;
    s_canvas = NULL;
    s_overlay = NULL;
    s_frame = NULL;

    prefs_load();

    s_title = ui_label(root, &sol_font_20, UI_TITLE, "Pong");
    lv_obj_set_pos(s_title, 8, 2);

    s_score_lab = ui_label(root, &sol_mono_12, UI_SCORE, "");
    lv_obj_set_pos(s_score_lab, 150, 6);

    s_best_lab = ui_label(root, &sol_mono_12, UI_BEST, "");
    lv_obj_set_pos(s_best_lab, 230, 6);
    refresh_hud();

    {
        const int sy = HUD_H - 3;
        lv_obj_t *a = lv_obj_create(root);
        lv_obj_remove_style_all(a);
        lv_obj_set_size(a, 100, 3);
        lv_obj_set_pos(a, 8, sy);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(a, lv_color_hex(UI_TITLE), 0);
        lv_obj_t *b = lv_obj_create(root);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 100, 3);
        lv_obj_set_pos(b, 110, sy);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_SCORE), 0);
        lv_obj_t *c = lv_obj_create(root);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, 100, 3);
        lv_obj_set_pos(c, 212, sy);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(UI_BEST), 0);
    }

    size_t bytes = (size_t)COURT_W * COURT_H * 2;
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
    s_dsc.header.w = COURT_W;
    s_dsc.header.h = COURT_H;
    s_dsc.header.stride = COURT_W * 2;
    s_dsc.data_size = (uint32_t)bytes;
    s_dsc.data = (const uint8_t *)s_fb;

    int gx = (OS_ROOT_W - COURT_W) / 2;
    int gy = HUD_H + 2;

    s_frame = ui_panel(root, gx - FRAME_PAD, gy - FRAME_PAD,
                       COURT_W + 2 * FRAME_PAD, COURT_H + 2 * FRAME_PAD);
    lv_obj_set_style_pad_all(s_frame, 0, 0);
    lv_obj_set_style_bg_color(s_frame, lv_color_hex(UI_BOX_EDGE), 0);
    lv_obj_set_style_border_width(s_frame, 0, 0);

    s_canvas = lv_image_create(root);
    lv_image_set_src(s_canvas, &s_dsc);
    lv_obj_set_pos(s_canvas, gx, gy);

    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, COURT_W - 24, 52);
    lv_obj_set_pos(s_overlay, gx + 12, gy + (COURT_H - 52) / 2);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(UI_BOX_BG), 0);
    lv_obj_set_style_border_width(s_overlay, 3, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(UI_BOX_EDGE), 0);
    lv_obj_set_style_radius(s_overlay, 6, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_t *ot = ui_label(s_overlay, &sol_font_20, UI_BOX_INK, "");
    lv_obj_set_width(ot, COURT_W - 24);
    lv_obj_set_style_text_align(ot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *os = ui_label(s_overlay, &sol_mono_12, UI_BEST, "");
    lv_obj_set_width(os, COURT_W - 24);
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
    physics_step();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (s_state == ST_IDLE) {
        if (k == OS_KEY_SELECT && !repeat) {
            start_game();
            return true;
        }
        if (k == OS_KEY_LEFT && !repeat) {
            cycle_mode(-1);
            return true;
        }
        if (k == OS_KEY_RIGHT && !repeat) {
            cycle_mode(1);
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            return false; /* exit */
        }
        return true;
    }

    if (s_state == ST_OVER) {
        if (k == OS_KEY_SELECT && !repeat) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            go_idle();
            return true;
        }
        return true;
    }

    if (s_state == ST_PAUSE) {
        if (k == OS_KEY_SELECT && !repeat) {
            resume_play();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            best_save();
            go_idle();
            return true;
        }
        return true;
    }

    /* ST_PLAY — allow key repeat for paddle hold */
    if (k == OS_KEY_BACK && !repeat) {
        go_pause();
        return true;
    }
    if (k == OS_KEY_UP) {
        move_paddle(&s_p1_y, -PADDLE_STEP);
        redraw();
        return true;
    }
    if (k == OS_KEY_DOWN) {
        move_paddle(&s_p1_y, PADDLE_STEP);
        redraw();
        return true;
    }
    if (s_mode == MODE_2P) {
        if (k == OS_KEY_LEFT) {
            move_paddle(&s_p2_y, -PADDLE_STEP);
            redraw();
            return true;
        }
        if (k == OS_KEY_RIGHT) {
            move_paddle(&s_p2_y, PADDLE_STEP);
            redraw();
            return true;
        }
    }
    return true;
}

const os_app_t app_pong = {
    .id = "pong",
    .name = "Pong",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
