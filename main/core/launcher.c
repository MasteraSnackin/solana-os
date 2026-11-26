// App browser: paged 3x2 grid of every registered app (but those whose
// visible() says no, e.g. Beacon on a badge that isn't one).
//
// Visual theme: original "Scott vs World" comic / fight-roster vibe —
// ink panels, paper cream, electric teal/pink accents, selection bounce,
// speed-line pulse, VS subtitle flash. Inspired by, not copying, any
// movie/comic assets or trademarks.

#include <string.h>
#include "os.h"
#include "ui.h"
#include "bsp.h"

#define COLS        3
#define ROWS        2
#define PER_PAGE    (COLS * ROWS)
#define TILE_W      96
#define TILE_H      72
#define GAP         6
#define BANNER_H    18
#define X0          ((OS_ROOT_W - COLS * TILE_W - (COLS - 1) * GAP) / 2)
#define Y0          (BANNER_H + 6)
#define STRIPE_H    5
#define SPEED_N     5
#define BOUNCE_PX   5
#define BOUNCE_TICKS 4
#define VS_TICKS    6
#define LED_TICKS   5

/* Comic / fight-roster palette (original; not remapped by ui_col). */
#define T_INK           0x0B0B0B
#define T_PAPER         0xF3EBDD
#define T_CREAM         0xFFF8E8
#define T_FACE          0xFFFDF6
#define T_TEAL          0x00D4C8
#define T_PINK          0xFF2D6A
#define T_INK_SOFT      0x2A2A2A
#define T_HALFTONE      0xD9CFC0

/* Slot accents: teal / pink only — high-contrast comic accents. */
static const uint32_t s_slot_accent[PER_PAGE] = {
    T_PINK, T_TEAL, T_PINK, T_TEAL, T_PINK, T_TEAL
};

static lv_obj_t *s_root, *s_tiles[PER_PAGE], *s_stripes[PER_PAGE];
static lv_obj_t *s_icons[PER_PAGE], *s_names[PER_PAGE];
static lv_obj_t *s_dots, *s_hint_bar, *s_hint;
static lv_obj_t *s_banner, *s_banner_lbl, *s_vs, *s_subtitle;
static lv_obj_t *s_speed[SPEED_N];
static int s_tile_base_y[PER_PAGE];
static int s_sel, s_page = -1;
static const os_app_t *s_list[64];          // the apps shown, in registry order
static int s_count;

/* Motion state — driven from on_tick (OS_TICK_MS = 100). Keep light for C5. */
static int s_bounce_left;       /* ticks remaining on focus bounce */
static int s_vs_left;           /* ticks remaining on VS flash */
static int s_led_left;          /* ticks remaining on WS2812 pulse */
static int s_pulse_phase;       /* speed-line / halftone phase */
static int s_focus_slot = -1;   /* slot index on current page, or -1 */

static bool listed(const os_app_t *app)
{
    return !app->visible || app->visible();
}

// Rebuilds the list; true if it changed. The selection stays on its app.
static bool collect(void)
{
    const os_app_t *sel = s_count ? s_list[s_sel < s_count ? s_sel : 0] : NULL;
    const os_app_t *next[64];
    int n = 0;
    for (int i = 0; i < os_app_count && n < 64; i++) {
        if (listed(os_apps[i])) {
            next[n++] = os_apps[i];
        }
    }
    if (n == s_count && !memcmp(next, s_list, n * sizeof(next[0]))) {
        return false;
    }
    memcpy(s_list, next, n * sizeof(next[0]));
    s_count = n;
    s_sel = 0;
    for (int i = 0; i < n; i++) {
        if (s_list[i] == sel) {
            s_sel = i;
        }
    }
    return true;
}

static void leds_off(void)
{
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
}

static void leds_pulse(uint32_t col)
{
    uint8_t r = ((col >> 16) & 0xFF) * OS_LED_MAX / 255;
    uint8_t g = ((col >> 8) & 0xFF) * OS_LED_MAX / 255;
    uint8_t b = (col & 0xFF) * OS_LED_MAX / 255;
    bsp_rgb_fill(r, g, b);
    bsp_rgb_show();
}

static void kick_focus_fx(void)
{
    s_bounce_left = BOUNCE_TICKS;
    s_vs_left = VS_TICKS;
    s_led_left = LED_TICKS;
    s_pulse_phase = 0;

    if (s_vs) {
        lv_obj_set_hidden(s_vs, false);
        lv_obj_set_style_text_opa(s_vs, LV_OPA_COVER, 0);
    }
    if (s_subtitle && s_sel < s_count) {
        lv_label_set_text(s_subtitle, s_list[s_sel]->name);
        lv_obj_set_hidden(s_subtitle, false);
    }
    leds_pulse(T_PINK);
}

static void style_tile(int slot, const os_app_t *app, bool focused)
{
    lv_obj_t *t = s_tiles[slot];
    uint32_t accent = s_slot_accent[slot];
    uint32_t fill = focused ? T_CREAM : T_FACE;
    uint32_t border = focused ? T_PINK : T_INK;
    uint32_t ink = T_INK;
    uint32_t icon_col = focused ? T_PINK : accent;

    lv_obj_set_style_bg_color(t, lv_color_hex(fill), 0);
    lv_obj_set_style_border_width(t, focused ? 3 : 2, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(border), 0);

    if (s_stripes[slot]) {
        lv_obj_set_style_bg_color(s_stripes[slot],
                                  lv_color_hex(focused ? T_TEAL : accent), 0);
        /* Halftone-ish pulse: stripe height nudge via opa only (cheap). */
        lv_obj_set_style_bg_opa(s_stripes[slot],
                                focused ? LV_OPA_COVER : LV_OPA_90, 0);
    }

    lv_obj_set_style_text_color(s_names[slot], lv_color_hex(ink), 0);
    if (app->symbol) {
        lv_obj_set_style_text_color(s_icons[slot], lv_color_hex(icon_col), 0);
    } else {
        lv_obj_set_style_image_recolor(s_icons[slot], lv_color_hex(T_PINK), 0);
        lv_obj_set_style_image_recolor_opa(s_icons[slot],
                                           focused ? LV_OPA_60 : LV_OPA_TRANSP, 0);
    }
}

static void layout_speed_lines(int focus_slot)
{
    if (focus_slot < 0 || focus_slot >= PER_PAGE) {
        for (int i = 0; i < SPEED_N; i++) {
            if (s_speed[i]) {
                lv_obj_set_hidden(s_speed[i], true);
            }
        }
        return;
    }
    int tx = X0 + (focus_slot % COLS) * (TILE_W + GAP);
    int ty = s_tile_base_y[focus_slot];
    /* Short ink speed-lines flanking the focused panel. */
    static const int16_t offs[SPEED_N] = { -10, -4, 8, 18, 28 };
    static const int16_t widths[SPEED_N] = { 28, 40, 22, 36, 18 };
    for (int i = 0; i < SPEED_N; i++) {
        if (!s_speed[i]) {
            continue;
        }
        int w = widths[i];
        int x = (focus_slot % COLS) == 0 ? tx + TILE_W + 2
              : (focus_slot % COLS) == COLS - 1 ? tx - w - 2
              : tx + TILE_W + 2;
        if ((focus_slot % COLS) == 1 && (i & 1)) {
            x = tx - w - 2;
        }
        lv_obj_set_hidden(s_speed[i], false);
        lv_obj_set_pos(s_speed[i], x, ty + 16 + offs[i]);
        lv_obj_set_size(s_speed[i], w, i == 2 ? 3 : 2);
    }
}

static void apply_bounce(void)
{
    for (int i = 0; i < PER_PAGE; i++) {
        if (!s_tiles[i] || lv_obj_is_hidden(s_tiles[i])) {
            continue;
        }
        int y = s_tile_base_y[i];
        if (i == s_focus_slot && s_bounce_left > 0) {
            /* Ease: full lift then settle. */
            int lift = (s_bounce_left > BOUNCE_TICKS / 2)
                     ? BOUNCE_PX
                     : (BOUNCE_PX * s_bounce_left) / (BOUNCE_TICKS / 2 + 1);
            y -= lift;
        }
        lv_obj_set_y(s_tiles[i], y);
    }
}

static void render(void)
{
    int page = s_sel / PER_PAGE;
    if (page != s_page) {
        s_page = page;
        for (int i = 0; i < PER_PAGE; i++) {
            int idx = page * PER_PAGE + i;
            lv_obj_t *t = s_tiles[i];
            if (idx >= s_count) {
                lv_obj_set_hidden(t, true);
                continue;
            }
            const os_app_t *app = s_list[idx];
            lv_obj_set_hidden(t, false);
            lv_label_set_text(s_names[i], app->name);
            // Icons are either a symbol label or an image; swap the widget as needed.
            if (s_icons[i]) {
                lv_obj_delete(s_icons[i]);
            }
            if (app->symbol) {
                s_icons[i] = lv_label_create(t);
                lv_obj_set_style_text_font(s_icons[i], &lv_font_montserrat_20, 0);
                lv_label_set_text(s_icons[i], app->symbol);
            } else {
                s_icons[i] = lv_image_create(t);
                lv_image_set_src(s_icons[i], app->icon);
            }
            lv_obj_align(s_icons[i], LV_ALIGN_CENTER, 0, -6);
        }
        int pages = (s_count + PER_PAGE - 1) / PER_PAGE;
        lv_obj_clean(s_dots);
        lv_obj_set_hidden(s_dots, pages < 2);
        for (int p = 0; p < pages; p++) {
            lv_obj_t *d = lv_obj_create(s_dots);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, p == page ? 14 : 6, 6);
            lv_obj_set_style_radius(d, 0, 0);   /* comic squares, not pills */
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(d,
                                     lv_color_hex(p == page ? T_PINK : T_INK_SOFT), 0);
            if (p == page) {
                lv_obj_set_style_border_width(d, 1, 0);
                lv_obj_set_style_border_color(d, lv_color_hex(T_INK), 0);
            }
        }
    }

    s_focus_slot = (s_sel >= 0 && s_sel < s_count) ? (s_sel % PER_PAGE) : -1;
    for (int i = 0; i < PER_PAGE; i++) {
        int idx = page * PER_PAGE + i;
        if (idx < s_count) {
            style_tile(i, s_list[idx], idx == s_sel);
        }
    }
    layout_speed_lines(s_focus_slot);
    apply_bounce();

    if (s_subtitle && s_sel < s_count && s_vs_left <= 0) {
        lv_label_set_text(s_subtitle, s_list[s_sel]->name);
        lv_obj_set_hidden(s_subtitle, false);
    }
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_page = -1;
    s_bounce_left = 0;
    s_vs_left = 0;
    s_led_left = 0;
    s_pulse_phase = 0;
    s_focus_slot = -1;
    collect();
    if (s_sel >= s_count) {
        s_sel = 0;
    }

    /* Paper stage — comic page. */
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(T_PAPER), 0);

    /* Top fight banner. */
    s_banner = lv_obj_create(root);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_pos(s_banner, 0, 0);
    lv_obj_set_size(s_banner, OS_ROOT_W, BANNER_H);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(T_INK), 0);
    lv_obj_set_style_border_side(s_banner, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_banner, 2, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_hex(T_TEAL), 0);
    lv_obj_set_scrollable(s_banner, false);

    s_banner_lbl = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_lbl, &sol_mono_12, 0);
    lv_obj_set_style_text_color(s_banner_lbl, lv_color_hex(T_TEAL), 0);
    lv_label_set_text(s_banner_lbl, "LEVEL SELECT  //  BADGE VS WORLD");
    lv_obj_align(s_banner_lbl, LV_ALIGN_LEFT_MID, 8, 0);

    /* Speed-line décor (repositioned on focus). */
    for (int i = 0; i < SPEED_N; i++) {
        lv_obj_t *ln = lv_obj_create(root);
        lv_obj_remove_style_all(ln);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ln, lv_color_hex(T_INK), 0);
        lv_obj_set_scrollable(ln, false);
        lv_obj_set_hidden(ln, true);
        s_speed[i] = ln;
    }

    for (int i = 0; i < PER_PAGE; i++) {
        int x = X0 + (i % COLS) * (TILE_W + GAP);
        int y = Y0 + (i / COLS) * (TILE_H + GAP);
        s_tile_base_y[i] = y;

        /* Thick ink comic panel. */
        lv_obj_t *t = lv_obj_create(root);
        lv_obj_remove_style_all(t);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(T_FACE), 0);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(T_INK), 0);
        lv_obj_set_style_radius(t, 2, 0);   /* near-sharp comic corners */
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_scrollable(t, false);
        s_tiles[i] = t;

        /* Accent header stripe. */
        lv_obj_t *stripe = lv_obj_create(t);
        lv_obj_remove_style_all(stripe);
        lv_obj_set_pos(stripe, 0, 0);
        lv_obj_set_size(stripe, TILE_W, STRIPE_H);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(stripe, lv_color_hex(s_slot_accent[i]), 0);
        lv_obj_set_scrollable(stripe, false);
        s_stripes[i] = stripe;

        s_icons[i] = NULL;

        lv_obj_t *name = lv_label_create(t);
        lv_obj_set_style_text_font(name, &sol_font_14, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(T_INK), 0);
        lv_label_set_text(name, "");
        lv_obj_set_width(name, TILE_W - 8);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -3);
        s_names[i] = name;
    }

    /* VS flash + fight subtitle (over the grid, under banner). */
    s_vs = lv_label_create(root);
    lv_obj_set_style_text_font(s_vs, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_vs, lv_color_hex(T_PINK), 0);
    lv_label_set_text(s_vs, "VS");
    lv_obj_align(s_vs, LV_ALIGN_TOP_RIGHT, -10, 1);
    lv_obj_set_hidden(s_vs, true);

    s_subtitle = lv_label_create(root);
    lv_obj_set_style_text_font(s_subtitle, &sol_mono_12, 0);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(T_PINK), 0);
    lv_label_set_text(s_subtitle, "");
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_RIGHT, -36, 4);
    lv_obj_set_hidden(s_subtitle, true);

    s_dots = lv_obj_create(root);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, 120, 8);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -UI_HINT_H - 4);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 4, 0);

    /* Ink hint bar with teal top rule. */
    s_hint_bar = lv_obj_create(root);
    lv_obj_remove_style_all(s_hint_bar);
    lv_obj_set_size(s_hint_bar, OS_ROOT_W, UI_HINT_H);
    lv_obj_align(s_hint_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_hint_bar, lv_color_hex(T_INK), 0);
    lv_obj_set_style_border_side(s_hint_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_hint_bar, 2, 0);
    lv_obj_set_style_border_color(s_hint_bar, lv_color_hex(T_PINK), 0);
    lv_obj_set_scrollable(s_hint_bar, false);

    s_hint = lv_label_create(s_hint_bar);
    lv_obj_set_style_text_font(s_hint, &sol_mono_12, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(T_CREAM), 0);
    lv_label_set_text(s_hint,
                      LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                      " move   SELECT fight   BOOT home");
    lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 6, 0);

    render();
    kick_focus_fx();
}

static void exit_app(void)
{
    leds_off();
    s_bounce_left = 0;
    s_vs_left = 0;
    s_led_left = 0;
    s_root = NULL;
    for (int i = 0; i < SPEED_N; i++) {
        s_speed[i] = NULL;
    }
    s_banner = s_banner_lbl = s_vs = s_subtitle = NULL;
    s_dots = s_hint_bar = s_hint = NULL;
    for (int i = 0; i < PER_PAGE; i++) {
        s_tiles[i] = s_stripes[i] = s_icons[i] = s_names[i] = NULL;
    }
}

static bool key(os_key_t k, bool repeat)
{
    int n = s_sel;
    switch (k) {
    case OS_KEY_LEFT:  n--; break;
    case OS_KEY_RIGHT: n++; break;
    case OS_KEY_UP:    n -= COLS; break;
    case OS_KEY_DOWN:  n += COLS; break;
    case OS_KEY_SELECT:
        if (s_sel < s_count) {
            leds_pulse(T_TEAL);
            os_launch(s_list[s_sel]);
        }
        return true;
    default:
        return true;    // Back on the home screen does nothing
    }
    if (n < 0) {
        n = repeat ? 0 : (k == OS_KEY_LEFT ? s_count - 1 : s_sel);
    } else if (n >= s_count) {
        n = repeat ? s_count - 1 : (k == OS_KEY_RIGHT ? 0 : s_count - 1);
    }
    if (n != s_sel) {
        s_sel = n;
        render();
        kick_focus_fx();
    }
    return true;
}

// An app showing up or going (a badge paired as a beacon) redraws the grid.
// Also advances the 2–3 intentional motions (bounce, speed-lines, VS/LED).
static void tick(void)
{
    if (collect()) {
        s_page = -1;
        render();
        kick_focus_fx();
    }

    bool animating = false;

    if (s_bounce_left > 0) {
        s_bounce_left--;
        apply_bounce();
        animating = true;
    }

    if (s_vs_left > 0) {
        s_vs_left--;
        if (s_vs) {
            /* Blink VS for a few ticks, then settle on subtitle only. */
            bool show = (s_vs_left > VS_TICKS / 2) || (s_vs_left & 1);
            lv_obj_set_hidden(s_vs, !show);
            lv_obj_set_style_text_opa(s_vs,
                                      s_vs_left > 1 ? LV_OPA_COVER : LV_OPA_50, 0);
        }
        if (s_vs_left == 0 && s_vs) {
            lv_obj_set_hidden(s_vs, true);
        }
        animating = true;
    }

    if (s_led_left > 0) {
        s_led_left--;
        if (s_led_left == LED_TICKS - 2) {
            leds_pulse(T_TEAL);
        } else if (s_led_left == 0) {
            leds_off();
        }
        animating = true;
    }

    /* Continuous gentle speed-line / stripe pulse (~2 Hz at 100 ms ticks). */
    s_pulse_phase = (s_pulse_phase + 1) & 7;
    lv_opa_t line_opa = (s_pulse_phase < 4)
                      ? (LV_OPA_40 + s_pulse_phase * 20)
                      : (LV_OPA_COVER - (s_pulse_phase - 4) * 20);
    for (int i = 0; i < SPEED_N; i++) {
        if (s_speed[i] && !lv_obj_is_hidden(s_speed[i])) {
            lv_obj_set_style_bg_opa(s_speed[i], line_opa, 0);
        }
    }
    if (s_focus_slot >= 0 && s_stripes[s_focus_slot]) {
        lv_opa_t stripe_opa = (s_pulse_phase & 1) ? LV_OPA_COVER : LV_OPA_80;
        lv_obj_set_style_bg_opa(s_stripes[s_focus_slot], stripe_opa, 0);
    }

    (void)animating;
}

const os_app_t os_launcher = {
    .id = "launcher",
    .name = "Apps",
    .on_enter = enter,
    .on_exit = exit_app,
    .on_key = key,
    .on_tick = tick,
};
