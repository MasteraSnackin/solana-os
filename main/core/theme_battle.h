#pragma once

// Shared pocket-battle UI palette for Solana OS apps on the BP26 badge.
// Bright primaries + cream tiles / panels. Original hexes — not remapped by
// ui_col() light/dark map. No copyrighted creature art, logos, or names.
//
// Use lv_color_hex(T_*) (or the helpers below) for battle-themed chrome.
// Leave the status bar alone so unthemed apps keep default shell chrome.

#include "lvgl.h"
#include "core/ui.h"

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
#define T_SOFT          0xE8F7FF
#define T_WHITE         0xFFFFFF
#define T_DIM           0x4A5568

#define BATTLE_STRIPE_H 6

// Sky stage behind app content (OS root is otherwise COL_BG).
static inline void battle_paint_stage(lv_obj_t *root)
{
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(T_SKY), 0);
}

// Cream face, thick navy border, optional coloured header stripe.
static inline lv_obj_t *battle_card(lv_obj_t *parent, int x, int y, int w, int h,
                                   uint32_t stripe_col)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(T_FACE), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(T_NAVY), 0);
    lv_obj_set_scrollable(card, false);

    if (stripe_col) {
        lv_obj_t *stripe = lv_obj_create(card);
        lv_obj_remove_style_all(stripe);
        lv_obj_set_pos(stripe, 0, 0);
        lv_obj_set_size(stripe, w, BATTLE_STRIPE_H);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(stripe, lv_color_hex(stripe_col), 0);
        lv_obj_set_style_radius(stripe, 0, 0);
        lv_obj_set_scrollable(stripe, false);
    }
    return card;
}

// Cream hint bar with navy top rule (replaces ui_hint for themed apps).
static inline lv_obj_t *battle_hint(lv_obj_t *root, const char *text)
{
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, OS_ROOT_W, UI_HINT_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(T_CREAM), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(T_NAVY), 0);
    lv_obj_set_scrollable(bar, false);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl, &sol_mono_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(T_NAVY), 0);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    return lbl;     /* callers keep the label; bar is its parent */
}

static inline void battle_set_hint(lv_obj_t *hint_lbl, const char *text)
{
    if (hint_lbl) {
        lv_label_set_text(hint_lbl, text ? text : "");
    }
}

static inline lv_obj_t *battle_label(lv_obj_t *parent, const lv_font_t *font,
                                    uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}
