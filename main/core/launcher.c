// App browser: paged 3x2 grid of every registered app (but those whose
// visible() says no, e.g. Beacon on a badge that isn't one).
//
// Visual theme: pocket-battle inspired (bright primaries, playful high-
// contrast tiles, simple panel chrome). Original palette — no copyrighted
// creature art, logos, or character names.

#include <string.h>
#include "os.h"
#include "ui.h"

#define COLS        3
#define ROWS        2
#define PER_PAGE    (COLS * ROWS)
#define TILE_W      96
#define TILE_H      80
#define GAP         8
#define X0          ((OS_ROOT_W - COLS * TILE_W - (COLS - 1) * GAP) / 2)
#define Y0          10
#define STRIPE_H    6

/* Pocket-battle palette (original hexes; not remapped by ui_col light map). */
#define T_SKY           0x5EC8F0
#define T_NAVY          0x1A2A6C
#define T_CREAM         0xFFF8E0
#define T_FACE          0xFFFDF5
#define T_INK           0x1A1A2E
#define T_RED           0xE63946
#define T_YELLOW        0xFFD60A
#define T_BLUE          0x3A86FF
#define T_TEAL          0x2EC4B6
#define T_ORANGE        0xFF9F1C

/* Per-slot accent stripe on unfocused tiles (cycles bright primaries). */
static const uint32_t s_slot_accent[PER_PAGE] = {
    T_RED, T_YELLOW, T_BLUE, T_TEAL, T_ORANGE, T_BLUE
};

static lv_obj_t *s_root, *s_tiles[PER_PAGE], *s_stripes[PER_PAGE];
static lv_obj_t *s_icons[PER_PAGE], *s_names[PER_PAGE];
static lv_obj_t *s_dots, *s_hint_bar, *s_hint;
static int s_sel, s_page = -1;
static const os_app_t *s_list[64];          // the apps shown, in registry order
static int s_count;

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

static void style_tile(int slot, const os_app_t *app, bool focused)
{
    lv_obj_t *t = s_tiles[slot];
    uint32_t accent = s_slot_accent[slot];
    uint32_t fill = focused ? T_YELLOW : T_FACE;
    uint32_t border = focused ? T_RED : T_NAVY;
    uint32_t ink = focused ? T_INK : T_NAVY;
    uint32_t icon_col = focused ? T_INK : accent;

    lv_obj_set_style_bg_color(t, lv_color_hex(fill), 0);
    lv_obj_set_style_border_width(t, focused ? 3 : 2, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(border), 0);

    if (s_stripes[slot]) {
        lv_obj_set_style_bg_color(s_stripes[slot],
                                  lv_color_hex(focused ? T_RED : accent), 0);
    }

    lv_obj_set_style_text_color(s_names[slot], lv_color_hex(ink), 0);
    if (app->symbol) {
        lv_obj_set_style_text_color(s_icons[slot], lv_color_hex(icon_col), 0);
    } else {
        /* Recolor image logos to ink when focused for contrast on yellow. */
        lv_obj_set_style_image_recolor(s_icons[slot], lv_color_hex(T_INK), 0);
        lv_obj_set_style_image_recolor_opa(s_icons[slot],
                                           focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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
            lv_obj_align(s_icons[i], LV_ALIGN_CENTER, 0, -8);
        }
        int pages = (s_count + PER_PAGE - 1) / PER_PAGE;
        lv_obj_clean(s_dots);
        lv_obj_set_hidden(s_dots, pages < 2);
        for (int p = 0; p < pages; p++) {
            lv_obj_t *d = lv_obj_create(s_dots);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, p == page ? 16 : 6, 6);
            lv_obj_set_style_radius(d, 3, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(d,
                                     lv_color_hex(p == page ? T_YELLOW : T_NAVY), 0);
            if (p == page) {
                lv_obj_set_style_border_width(d, 1, 0);
                lv_obj_set_style_border_color(d, lv_color_hex(T_RED), 0);
            }
        }
    }
    for (int i = 0; i < PER_PAGE; i++) {
        int idx = page * PER_PAGE + i;
        if (idx < s_count) {
            style_tile(i, s_list[idx], idx == s_sel);
        }
    }
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_page = -1;
    collect();
    if (s_sel >= s_count) {
        s_sel = 0;
    }

    /* Sky stage behind the grid. */
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(T_SKY), 0);

    for (int i = 0; i < PER_PAGE; i++) {
        int x = X0 + (i % COLS) * (TILE_W + GAP);
        int y = Y0 + (i / COLS) * (TILE_H + GAP);

        /* Thick navy chrome panel. */
        lv_obj_t *t = lv_obj_create(root);
        lv_obj_remove_style_all(t);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(T_FACE), 0);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(T_NAVY), 0);
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_scrollable(t, false);
        s_tiles[i] = t;

        /* Coloured header stripe (slot accent). */
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
        lv_obj_set_style_text_color(name, lv_color_hex(T_NAVY), 0);
        lv_label_set_text(name, "");
        lv_obj_set_width(name, TILE_W - 8);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);
        s_names[i] = name;
    }

    s_dots = lv_obj_create(root);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, 120, 8);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -UI_HINT_H - 6);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 4, 0);

    /* Cream hint bar with navy top rule. */
    s_hint_bar = lv_obj_create(root);
    lv_obj_remove_style_all(s_hint_bar);
    lv_obj_set_size(s_hint_bar, OS_ROOT_W, UI_HINT_H);
    lv_obj_align(s_hint_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_hint_bar, lv_color_hex(T_CREAM), 0);
    lv_obj_set_style_border_side(s_hint_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_hint_bar, 2, 0);
    lv_obj_set_style_border_color(s_hint_bar, lv_color_hex(T_NAVY), 0);
    lv_obj_set_scrollable(s_hint_bar, false);

    s_hint = lv_label_create(s_hint_bar);
    lv_obj_set_style_text_font(s_hint, &sol_mono_12, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(T_NAVY), 0);
    lv_label_set_text(s_hint,
                      LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                      " move   SELECT open   BOOT home");
    lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 6, 0);

    render();
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
    s_sel = n;
    render();
    return true;
}

// An app showing up or going (a badge paired as a beacon) redraws the grid.
static void tick(void)
{
    if (collect()) {
        s_page = -1;
        render();
    }
}

const os_app_t os_launcher = {
    .id = "launcher",
    .name = "Apps",
    .on_enter = enter,
    .on_key = key,
    .on_tick = tick,
};
