// Dino: side-scrolling desert runner for the BP26 badge (320×240).
// Auto-run; UP/SELECT jump; DOWN duck; CANCEL pause / exit. Score grows with
// distance; obstacles (spires + soarers) speed up over time. Night mode every
// 700 pts; top-5 high-score board in NVS. Original pixel art only — no Chrome /
// Google assets or marks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "nvs.h"

#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"

#define NVS_NS          "dino"
#define NVS_BEST        "best"
#define HI_N            5

#define HUD_H           20
#define FIELD_W         320
#define FIELD_H         168             /* fits under HUD + hint on 218-tall root */
#define GROUND_Y        (FIELD_H - 18)  /* top of ground strip */
#define RUNNER_X        36

#define MAX_OBS         6
#define SPEED_START     5
#define SPEED_MAX       14
#define JUMP_V0         (-13)
#define GRAVITY         2
#define NIGHT_EVERY     700             /* day↔night flip each N points */

/* Day — desert dusk */
#define DAY_SKY_TOP     0x3A6FA8
#define DAY_SKY_MID     0xC87848
#define DAY_SKY_LOW     0xE8B070
#define DAY_SAND        0xD8B070
#define DAY_SAND_DARK   0xB89050
#define DAY_GROUND      0x8A6840
#define DAY_LINE        0x5A4030
#define DAY_RUNNER      0x2A3040
#define DAY_RUNNER_EYE  0xF0F0E0
#define DAY_SPIRE       0x2E8A48
#define DAY_SPIRE_LIT   0x48B060
#define DAY_SOAR        0x4A3858
#define DAY_CLOUD       0xF0D8C0

/* Night — cool desert */
#define NGT_SKY_TOP     0x081828
#define NGT_SKY_MID     0x102848
#define NGT_SKY_LOW     0x183858
#define NGT_SAND        0x3A4860
#define NGT_SAND_DARK   0x2A3850
#define NGT_GROUND      0x1A2838
#define NGT_LINE        0x607088
#define NGT_RUNNER      0xE8E0C8
#define NGT_RUNNER_EYE  0x203040
#define NGT_SPIRE       0x58C070
#define NGT_SPIRE_LIT   0x88F0A0
#define NGT_SOAR        0xC0A8D0
#define NGT_STAR        0xF0F0D0

#define PX_DEAD_TINT    0x681818

#define UI_TITLE        0xC87848
#define UI_SCORE        0xE84828
#define UI_BEST         0x3A6FA8
#define UI_BOX_BG       0xF8F0D8
#define UI_BOX_INK      0x203060
#define UI_BOX_EDGE     0x8A6840

typedef enum {
    ST_IDLE = 0,
    ST_PLAY,
    ST_PAUSE,
    ST_OVER,
    ST_SCORES,
} state_t;

typedef enum {
    OBS_SPIRE = 0,      /* ground — jump */
    OBS_SOAR,           /* aerial — duck under (high) or jump (low) */
} obs_kind_t;

typedef struct {
    bool alive;
    obs_kind_t kind;
    int x;              /* left edge, world pixels relative to screen */
    int w, h;
    int y;              /* top of hitbox */
} obs_t;

static lv_obj_t *s_root, *s_title, *s_score_lab, *s_best_lab, *s_hint;
static lv_obj_t *s_canvas, *s_overlay;
static lv_image_dsc_t s_dsc;
static uint16_t *s_fb;
static state_t s_state;

static int s_score;
static int32_t s_best;
static int32_t s_hi[HI_N];
static int s_speed;             /* px per tick */
static int s_dist_acc;          /* leftover for score ticks */
static int s_spawn_cd;          /* ticks until next obstacle */
static int s_anim;              /* run-cycle frame */
static int s_ground_scroll;
static bool s_night;

static int s_y;                 /* runner feet Y (top of feet = GROUND_Y when grounded) */
static int s_vy;
static bool s_ducking;
static bool s_down_held;        /* refreshed by DOWN presses/repeats; cleared each tick if idle */
static bool s_down_seen;        /* DOWN observed since last on_tick */
static bool s_on_ground;
static obs_t s_obs[MAX_OBS];

// ---- colour / blit --------------------------------------------------------

static uint16_t color_u16(uint32_t rgb)
{
    return lv_color_to_u16(lv_color_hex(rgb));
}

static void put_px(int x, int y, uint16_t c)
{
    if (!s_fb || x < 0 || y < 0 || x >= FIELD_W || y >= FIELD_H) {
        return;
    }
    s_fb[y * FIELD_W + x] = c;
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t c)
{
    if (!s_fb) {
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

static void sync_night(void)
{
    s_night = ((s_score / NIGHT_EVERY) % 2) == 1;
}

static void fill_sky(void)
{
    uint32_t sky_top = s_night ? NGT_SKY_TOP : DAY_SKY_TOP;
    uint32_t sky_mid = s_night ? NGT_SKY_MID : DAY_SKY_MID;
    uint32_t sky_low = s_night ? NGT_SKY_LOW : DAY_SKY_LOW;
    uint32_t sand = s_night ? NGT_SAND : DAY_SAND;
    uint32_t sand_d = s_night ? NGT_SAND_DARK : DAY_SAND_DARK;
    uint32_t ground = s_night ? NGT_GROUND : DAY_GROUND;
    uint32_t line = s_night ? NGT_LINE : DAY_LINE;

    int a = FIELD_H / 3;
    int b = (FIELD_H * 2) / 3;
    fill_rect(0, 0, FIELD_W, a, color_u16(sky_top));
    fill_rect(0, a, FIELD_W, b - a, color_u16(sky_mid));
    fill_rect(0, b, FIELD_W, GROUND_Y - b, color_u16(sky_low));
    fill_rect(0, GROUND_Y, FIELD_W, FIELD_H - GROUND_Y, color_u16(ground));

    fill_rect(0, GROUND_Y - 4, FIELD_W, 4, color_u16(sand));
    fill_rect(0, GROUND_Y - 1, FIELD_W, 1, color_u16(line));

    uint16_t dash = color_u16(sand_d);
    for (int x = -((s_ground_scroll) % 24); x < FIELD_W; x += 24) {
        fill_rect(x, GROUND_Y + 6, 10, 2, dash);
    }

    if (s_night) {
        uint16_t star = color_u16(NGT_STAR);
        /* sparse twinkle field — deterministic from scroll */
        for (int i = 0; i < 18; i++) {
            int sx = (i * 53 + (s_ground_scroll / 4)) % FIELD_W;
            int sy = 6 + (i * 17) % (GROUND_Y / 2);
            put_px(sx, sy, star);
            if ((i & 3) == 0) {
                put_px(sx + 1, sy, star);
            }
        }
    } else {
        uint16_t cloud = color_u16(DAY_CLOUD);
        int cx = (FIELD_W - ((s_ground_scroll / 3) % (FIELD_W + 60))) - 40;
        fill_rect(cx, 18, 28, 6, cloud);
        fill_rect(cx + 8, 14, 18, 6, cloud);
        fill_rect(cx + 90, 28, 22, 5, cloud);
    }
}

// ---- original sprites (procedural pixels; not Chrome art) -----------------

static void draw_runner(int feet_x, int feet_y, bool duck, int frame)
{
    uint16_t body = color_u16(s_night ? NGT_RUNNER : DAY_RUNNER);
    uint16_t eye = color_u16(s_night ? NGT_RUNNER_EYE : DAY_RUNNER_EYE);

    if (duck) {
        /* low silhouette: long body, tucked head — matches duck hitbox */
        int top = feet_y - 12;
        fill_rect(feet_x, top + 3, 24, 9, body);
        fill_rect(feet_x + 20, top, 10, 9, body);           /* head forward */
        put_px(feet_x + 26, top + 2, eye);
        fill_rect(feet_x + 4, feet_y - 2, 4, 2, body);
        fill_rect(feet_x + 12, feet_y - 2, 4, 2, body);
        return;
    }

    int top = feet_y - 26;
    fill_rect(feet_x + 4, top + 8, 12, 14, body);
    fill_rect(feet_x + 10, top, 12, 10, body);
    fill_rect(feet_x + 20, top + 4, 6, 4, body);
    put_px(feet_x + 18, top + 3, eye);
    fill_rect(feet_x + 12, top - 3, 3, 3, body);
    fill_rect(feet_x + 16, top - 2, 2, 2, body);
    fill_rect(feet_x + 2, top + 12, 4, 3, body);
    if ((frame & 1) == 0) {
        fill_rect(feet_x + 6, feet_y - 6, 4, 6, body);
        fill_rect(feet_x + 14, feet_y - 4, 4, 4, body);
        fill_rect(feet_x + 18, feet_y - 2, 4, 2, body);
    } else {
        fill_rect(feet_x + 6, feet_y - 4, 4, 4, body);
        fill_rect(feet_x + 2, feet_y - 2, 4, 2, body);
        fill_rect(feet_x + 14, feet_y - 6, 4, 6, body);
    }
    fill_rect(feet_x - 6, top + 14, 10, 3, body);
    fill_rect(feet_x - 10, top + 12, 4, 3, body);
}

static void draw_spire(const obs_t *o)
{
    uint16_t a = color_u16(s_night ? NGT_SPIRE : DAY_SPIRE);
    uint16_t b = color_u16(s_night ? NGT_SPIRE_LIT : DAY_SPIRE_LIT);
    int x = o->x;
    int y = o->y;
    int w = o->w;
    int h = o->h;

    fill_rect(x + w / 3, y, w / 3 + 1, h, a);
    if (h > 22) {
        fill_rect(x, y + h / 3, w / 3 + 1, 4, a);
        fill_rect(x, y + h / 3 - 10, 3, 10, a);
    }
    if (h > 28) {
        fill_rect(x + 2 * w / 3, y + h / 2, w / 3 + 1, 4, a);
        fill_rect(x + w - 3, y + h / 2 - 8, 3, 8, a);
    }
    fill_rect(x + w / 3 + 1, y + 2, 1, h - 4, b);
}

static void draw_soar(const obs_t *o, int frame)
{
    uint16_t c = color_u16(s_night ? NGT_SOAR : DAY_SOAR);
    int x = o->x;
    int y = o->y;
    fill_rect(x + 6, y + 4, 14, 5, c);
    fill_rect(x + 20, y + 5, 5, 2, c);
    if ((frame & 1) == 0) {
        fill_rect(x, y, 10, 3, c);
        fill_rect(x + 16, y, 10, 3, c);
    } else {
        fill_rect(x, y + 6, 10, 3, c);
        fill_rect(x + 16, y + 6, 10, 3, c);
    }
}

/*
 * Duck hitbox polish: crouched AABB matches the low sprite (h=12 from feet),
 * not the old upright-ish 14×30 box that left empty air above the body and a
 * wide front that clipped high soarers unfairly. Standing stays 22×26.
 * Callers apply a small shrink for fairness.
 */
static void runner_hitbox(int *x, int *y, int *w, int *h)
{
    *x = RUNNER_X;
    if (s_ducking && s_on_ground) {
        *y = s_y - 12;
        *w = 26;
        *h = 12;
    } else {
        *y = s_y - 26;
        *w = 22;
        *h = 26;
    }
}

static bool rects_overlap(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static bool hit_test(void)
{
    int rx, ry, rw, rh;
    runner_hitbox(&rx, &ry, &rw, &rh);
    /* shrink for fairness — duck: more off the top (air above body) */
    if (s_ducking && s_on_ground) {
        rx += 2;
        ry += 3;
        rw -= 4;
        rh -= 4;
    } else {
        rx += 2;
        ry += 2;
        rw -= 4;
        rh -= 3;
    }
    if (rw < 4 || rh < 4) {
        return false;
    }
    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].alive) {
            continue;
        }
        obs_t *o = &s_obs[i];
        if (rects_overlap(rx, ry, rw, rh, o->x, o->y, o->w, o->h)) {
            return true;
        }
    }
    return false;
}

// ---- NVS / high-score board -----------------------------------------------

static void hi_sort_desc(void)
{
    for (int i = 0; i < HI_N - 1; i++) {
        for (int j = i + 1; j < HI_N; j++) {
            if (s_hi[j] > s_hi[i]) {
                int32_t t = s_hi[i];
                s_hi[i] = s_hi[j];
                s_hi[j] = t;
            }
        }
    }
}

/* Insert score into top-N if it qualifies. Returns rank 1..HI_N or 0. */
static int hi_insert(int32_t score)
{
    if (score <= 0) {
        return 0;
    }
    if (score <= s_hi[HI_N - 1] && s_hi[HI_N - 1] > 0) {
        return 0;
    }
    s_hi[HI_N - 1] = score;
    hi_sort_desc();
    for (int i = 0; i < HI_N; i++) {
        if (s_hi[i] == score) {
            return i + 1;
        }
    }
    return 0;
}

static void prefs_load(void)
{
    s_best = 0;
    memset(s_hi, 0, sizeof(s_hi));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, NVS_BEST, &v) == ESP_OK && v > 0) {
        s_best = v;
    }
    for (int i = 0; i < HI_N; i++) {
        char key[8];
        snprintf(key, sizeof(key), "hs%d", i);
        v = 0;
        if (nvs_get_i32(h, key, &v) == ESP_OK && v > 0) {
            s_hi[i] = v;
        }
    }
    nvs_close(h);
    hi_sort_desc();
    /* migrate: single best into board if board empty */
    if (s_best > 0 && s_hi[0] == 0) {
        s_hi[0] = s_best;
    }
    if (s_hi[0] > s_best) {
        s_best = s_hi[0];
    }
}

static void prefs_save(void)
{
    if (s_hi[0] > s_best) {
        s_best = s_hi[0];
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_BEST, s_best);
    for (int i = 0; i < HI_N; i++) {
        char key[8];
        snprintf(key, sizeof(key), "hs%d", i);
        nvs_set_i32(h, key, s_hi[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* Pause→title: update BEST only (incomplete run is not a board entry). */
static void best_only_save(void)
{
    if (s_score <= s_best) {
        return;
    }
    s_best = s_score;
    if (s_hi[0] < s_best) {
        /* keep board #1 in sync with BEST without inventing mid-run ranks */
        hi_insert(s_best);
    }
    prefs_save();
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

static void scores_overlay(void)
{
    char body[96];
    size_t n = 0;
    body[0] = '\0';
    for (int i = 0; i < HI_N; i++) {
        if (s_hi[i] <= 0) {
            n += (size_t)snprintf(body + n, sizeof(body) - n,
                                  "%d.  --\n", i + 1);
        } else {
            n += (size_t)snprintf(body + n, sizeof(body) - n,
                                  "%d.  %ld\n", i + 1, (long)s_hi[i]);
        }
        if (n >= sizeof(body)) {
            break;
        }
    }
    /* trim trailing newline for label */
    if (n > 0 && body[n - 1] == '\n') {
        body[n - 1] = '\0';
    }
    overlay_show(true, "TOP 5", body);
    set_hint("SELECT play  CANCEL title");
}

// ---- draw -----------------------------------------------------------------

static void redraw(void)
{
    if (!s_fb) {
        return;
    }

    sync_night();
    fill_sky();

    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].alive) {
            continue;
        }
        if (s_obs[i].kind == OBS_SPIRE) {
            draw_spire(&s_obs[i]);
        } else {
            draw_soar(&s_obs[i], s_anim);
        }
    }

    draw_runner(RUNNER_X, s_y, s_ducking && s_on_ground, s_anim);

    if (s_state == ST_OVER) {
        uint16_t tint = color_u16(PX_DEAD_TINT);
        for (int i = 0; i < FIELD_W * FIELD_H; i++) {
            uint16_t p = s_fb[i];
            s_fb[i] = (uint16_t)(((p & 0xF7DE) >> 1) + ((tint & 0xF7DE) >> 1));
        }
    }

    if (s_canvas) {
        lv_obj_invalidate(s_canvas);
    }

    if (s_root) {
        uint32_t bg = s_night ? NGT_SKY_TOP : DAY_SKY_TOP;
        lv_obj_set_style_bg_color(s_root, lv_color_hex(bg), 0);
    }
}

// ---- game -----------------------------------------------------------------

static void clear_obs(void)
{
    memset(s_obs, 0, sizeof(s_obs));
}

static obs_t *alloc_obs(void)
{
    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].alive) {
            return &s_obs[i];
        }
    }
    return NULL;
}

static void spawn_obstacle(void)
{
    obs_t *o = alloc_obs();
    if (!o) {
        return;
    }
    memset(o, 0, sizeof(*o));
    o->alive = true;
    o->x = FIELD_W + 4;

    uint32_t r = esp_random();
    /* ~65% ground spire, ~35% soarer */
    if ((r % 100) < 65) {
        o->kind = OBS_SPIRE;
        int style = (int)(r % 3);
        if (style == 0) {
            o->w = 10;
            o->h = 22;
        } else if (style == 1) {
            o->w = 14;
            o->h = 32;
        } else {
            o->w = 18;
            o->h = 40;
        }
        o->y = GROUND_Y - o->h;
    } else {
        o->kind = OBS_SOAR;
        o->w = 26;
        o->h = 12;
        /*
         * Bands vs polished hitboxes (after fairness shrink):
         *   stand ≈ [GROUND_Y-24, GROUND_Y), duck ≈ [GROUND_Y-9, GROUND_Y).
         * High occupies standing head-zone only → duck clears, stand hits.
         * Mid/low reach into duck zone → must jump.
         */
        int band = (int)((r >> 8) % 3);
        if (band == 0) {
            o->y = GROUND_Y - 30;       /* high — duck under */
        } else if (band == 1) {
            o->y = GROUND_Y - 20;       /* mid — jump */
        } else {
            o->y = GROUND_Y - 14;       /* low — jump */
        }
    }
}

static void idle_overlay(void)
{
    overlay_show(true, "DINO", "SELECT start · LEFT scores");
    set_hint("UP jump  DOWN duck  LEFT scores  CANCEL exit");
}

static void go_idle(void)
{
    s_state = ST_IDLE;
    s_score = 0;
    s_night = false;
    s_speed = SPEED_START;
    s_y = GROUND_Y;
    s_vy = 0;
    s_ducking = false;
    s_down_held = false;
    s_on_ground = true;
    s_anim = 0;
    clear_obs();
    obs_t *a = alloc_obs();
    if (a) {
        a->alive = true;
        a->kind = OBS_SPIRE;
        a->w = 14;
        a->h = 32;
        a->x = 200;
        a->y = GROUND_Y - a->h;
    }
    obs_t *b = alloc_obs();
    if (b) {
        b->alive = true;
        b->kind = OBS_SOAR;
        b->w = 26;
        b->h = 12;
        b->x = 260;
        b->y = GROUND_Y - 30;
    }
    refresh_hud();
    idle_overlay();
    redraw();
}

static void go_scores(void)
{
    s_state = ST_SCORES;
    scores_overlay();
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
    set_hint("UP jump  DOWN duck  CANCEL pause");
    redraw();
}

static void game_over(void)
{
    s_state = ST_OVER;
    bool is_new = s_score > s_best;
    int rank = hi_insert((int32_t)s_score);
    if (s_score > s_best) {
        s_best = s_score;
    }
    prefs_save();
    refresh_hud();
    char sub[56];
    if (is_new) {
        snprintf(sub, sizeof(sub), "NEW BEST %d — SELECT", s_score);
    } else if (rank > 0) {
        snprintf(sub, sizeof(sub), "#%d board · %d — SELECT", rank, s_score);
    } else {
        snprintf(sub, sizeof(sub), "score %d — SELECT again", s_score);
    }
    overlay_show(true, "GAME OVER", sub);
    set_hint("SELECT again  LEFT scores  CANCEL exit");
    os_toast("Game over — %d", s_score);
    redraw();
}

static void start_game(void)
{
    s_score = 0;
    s_night = false;
    s_dist_acc = 0;
    s_speed = SPEED_START;
    s_spawn_cd = 18;
    s_anim = 0;
    s_ground_scroll = 0;
    s_y = GROUND_Y;
    s_vy = 0;
    s_ducking = false;
    s_on_ground = true;
    clear_obs();
    s_state = ST_PLAY;
    refresh_hud();
    overlay_show(false, NULL, NULL);
    set_hint("UP jump  DOWN duck  CANCEL pause");
    redraw();
}

static void do_jump(void)
{
    if (s_on_ground && !s_ducking) {
        s_vy = JUMP_V0;
        s_on_ground = false;
    }
}

static void update_speed(void)
{
    int sp = SPEED_START + s_score / 100;
    if (sp > SPEED_MAX) {
        sp = SPEED_MAX;
    }
    s_speed = sp;
}

static void step(void)
{
    if (!s_down_seen) {
        s_down_held = false;
    }
    s_down_seen = false;
    s_ducking = s_down_held && s_on_ground;

    if (!s_on_ground) {
        s_y += s_vy;
        s_vy += GRAVITY;
        if (s_y >= GROUND_Y) {
            s_y = GROUND_Y;
            s_vy = 0;
            s_on_ground = true;
        }
    }

    s_ground_scroll += s_speed;
    s_anim++;

    s_dist_acc += s_speed;
    while (s_dist_acc >= 8) {
        s_dist_acc -= 8;
        s_score++;
    }
    update_speed();
    sync_night();
    refresh_hud();

    for (int i = 0; i < MAX_OBS; i++) {
        if (!s_obs[i].alive) {
            continue;
        }
        s_obs[i].x -= s_speed;
        if (s_obs[i].x + s_obs[i].w < -4) {
            s_obs[i].alive = false;
        }
    }

    if (s_spawn_cd > 0) {
        s_spawn_cd--;
    } else {
        spawn_obstacle();
        int gap = 28 - (s_speed - SPEED_START) * 2;
        if (gap < 16) {
            gap = 16;
        }
        s_spawn_cd = gap + (int)(esp_random() % 12);
    }

    if (hit_test()) {
        game_over();
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
    s_night = false;
    s_fb = NULL;
    s_canvas = NULL;
    s_overlay = NULL;
    s_down_held = false;

    prefs_load();

    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(DAY_SKY_TOP), 0);

    s_title = ui_label(root, &sol_font_20, UI_TITLE, "Dino");
    lv_obj_set_pos(s_title, 8, 1);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_TITLE), 0);

    s_score_lab = ui_label(root, &sol_mono_12, UI_SCORE, "");
    lv_obj_set_pos(s_score_lab, 100, 5);
    lv_obj_set_style_text_color(s_score_lab, lv_color_hex(UI_SCORE), 0);

    s_best_lab = ui_label(root, &sol_mono_12, UI_BEST, "");
    lv_obj_set_pos(s_best_lab, 210, 5);
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

    /* taller overlay for top-5 list */
    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, FIELD_W - 48, 96);
    lv_obj_set_pos(s_overlay, 24, gy + (FIELD_H - 96) / 2);
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
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *os = ui_label(s_overlay, &sol_mono_12, UI_BEST, "");
    lv_obj_set_width(os, FIELD_W - 48);
    lv_obj_set_style_text_align(os, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(os, lv_color_hex(UI_BEST), 0);
    lv_obj_align(os, LV_ALIGN_BOTTOM_MID, 0, -4);

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
    if (s_state != ST_PLAY) {
        return;
    }
    step();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (s_state == ST_PLAY && k == OS_KEY_DOWN) {
        s_down_held = true;
        s_down_seen = true;
        return true;
    }

    if (repeat) {
        return true;
    }

    if (s_state == ST_IDLE) {
        if (k == OS_KEY_SELECT || k == OS_KEY_UP) {
            start_game();
            if (k == OS_KEY_UP) {
                do_jump();
            }
            return true;
        }
        if (k == OS_KEY_LEFT) {
            go_scores();
            return true;
        }
        if (k == OS_KEY_BACK) {
            return false;
        }
        return true;
    }

    if (s_state == ST_SCORES) {
        if (k == OS_KEY_SELECT || k == OS_KEY_UP) {
            start_game();
            if (k == OS_KEY_UP) {
                do_jump();
            }
            return true;
        }
        if (k == OS_KEY_BACK || k == OS_KEY_LEFT) {
            go_idle();
            return true;
        }
        return true;
    }

    if (s_state == ST_OVER) {
        if (k == OS_KEY_SELECT || k == OS_KEY_UP) {
            start_game();
            if (k == OS_KEY_UP) {
                do_jump();
            }
            return true;
        }
        if (k == OS_KEY_LEFT) {
            go_scores();
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
            best_only_save();
            go_idle();
            return true;
        }
        return true;
    }

    if (s_state == ST_PLAY) {
        if (k == OS_KEY_BACK) {
            s_down_held = false;
            go_pause();
            return true;
        }
        if (k == OS_KEY_UP || k == OS_KEY_SELECT) {
            s_down_held = false;
            s_ducking = false;
            do_jump();
            return true;
        }
        return true;
    }

    return false;
}

const os_app_t app_dino = {
    .id = "dino",
    .name = "Dino",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_ORANGE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
