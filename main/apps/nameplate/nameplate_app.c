// Nameplate: show a badge wearer's name and title on the ILI9341 with a
// Contact QR that opens the live personal contact page. Name and title are
// editable on device and stored in NVS. Font size is cycleable (compact /
// normal / huge). The 8× WS2812 strip mirrors the name as a scrolling
// 5×7 column marquee while the app is open.

#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "core/os.h"
#include "core/theme_battle.h"
#include "core/ui.h"

#define CONTENT_H       (OS_ROOT_H - UI_HINT_H)
#define QR_BOX          148
#define QR_SIZE         132
#define COL_X           (10 + QR_BOX + 10)
#define COL_W           (OS_ROOT_W - COL_X - 8)

#define NVS_NS          "nameplate"
#define NAME_MAX        32
#define TITLE_MAX       40

#define CONTACT_PAGE_URL "https://masterasnackin.github.io/personal-contact-page/"

#define DEFAULT_NAME    "Dave Cheng"
#define DEFAULT_TITLE   "Get in touch"

#define LED_INK         0x2EC4B6    /* teal — matches nameplate accent */

enum { FIELD_NAME, FIELD_TITLE, FIELD_COUNT };

enum {
    FONT_COMPACT = 0,   /* name 20 / title 14 — more room for long strings */
    FONT_NORMAL,        /* name 32 / title 20 — default */
    FONT_HUGE,          /* name 32 full-bleed; QR hidden */
    FONT_COUNT
};

static const char *const font_mode_names[FONT_COUNT] = {
    "Compact", "Normal", "Huge",
};

static lv_obj_t *s_root, *s_view, *s_hint;
static lv_obj_t *s_name_lbl, *s_title_lbl, *s_field_hint, *s_font_lbl;
static ui_kb_t *s_kb;
static char s_name[NAME_MAX];
static char s_title[TITLE_MAX];
static int s_field;
static int s_font_mode = FONT_NORMAL;
static bool s_light;

/* ---- LED strip name mirror (5×7 column font, same idea as LED Badge) ---- */

static const uint8_t font_az[26][5] = {
    {0x3E,0x09,0x09,0x09,0x3E}, {0x3F,0x25,0x25,0x25,0x1A}, {0x1E,0x21,0x21,0x21,0x12},
    {0x3F,0x21,0x21,0x12,0x0C}, {0x3F,0x25,0x25,0x25,0x21}, {0x3F,0x05,0x05,0x05,0x01},
    {0x1E,0x21,0x25,0x25,0x1D}, {0x3F,0x04,0x04,0x04,0x3F}, {0x00,0x21,0x3F,0x21,0x00},
    {0x10,0x20,0x21,0x1F,0x01}, {0x3F,0x04,0x0A,0x11,0x20}, {0x3F,0x20,0x20,0x20,0x20},
    {0x3F,0x02,0x04,0x02,0x3F}, {0x3F,0x02,0x04,0x08,0x3F}, {0x1E,0x21,0x21,0x21,0x1E},
    {0x3F,0x09,0x09,0x09,0x06}, {0x1E,0x21,0x29,0x11,0x2E}, {0x3F,0x09,0x19,0x29,0x26},
    {0x22,0x25,0x25,0x25,0x19}, {0x01,0x01,0x3F,0x01,0x01}, {0x1F,0x20,0x20,0x20,0x1F},
    {0x0F,0x10,0x20,0x10,0x0F}, {0x1F,0x20,0x18,0x20,0x1F}, {0x31,0x0A,0x04,0x0A,0x31},
    {0x07,0x08,0x30,0x08,0x07}, {0x31,0x29,0x25,0x23,0x21},
};
static const uint8_t font_09[10][5] = {
    {0x1E,0x29,0x25,0x23,0x1E}, {0x00,0x22,0x3F,0x20,0x00}, {0x32,0x29,0x29,0x29,0x26},
    {0x12,0x21,0x25,0x25,0x1A}, {0x0C,0x0A,0x09,0x3F,0x08}, {0x27,0x25,0x25,0x25,0x19},
    {0x1E,0x25,0x25,0x25,0x18}, {0x01,0x39,0x05,0x03,0x01}, {0x1A,0x25,0x25,0x25,0x1A},
    {0x06,0x29,0x29,0x29,0x1E},
};
static const uint8_t font_sp[5] = {0, 0, 0, 0, 0};
static const uint8_t font_ex[5] = {0x00, 0x00, 0x2F, 0x00, 0x00};
static const uint8_t font_dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
static const uint8_t font_dot[5] = {0x00, 0x30, 0x30, 0x00, 0x00};

static int s_led_scroll;
static int s_led_tick;
static bool s_led_on;

static lv_obj_t *label_at(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          const char *text, int x, int y, int w)
{
    lv_obj_t *l = ui_label(parent, font, color, text);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    return l;
}

static const uint8_t *glyph_cols(char c, int *n)
{
    *n = 5;
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return font_az[c - 'A'];
    }
    if (c >= '0' && c <= '9') {
        return font_09[c - '0'];
    }
    if (c == '!') {
        return font_ex;
    }
    if (c == '-') {
        return font_dash;
    }
    if (c == '.' || c == ',') {
        return font_dot;
    }
    return font_sp;
}

static int name_content_width(void)
{
    int w = 0;
    for (const char *p = s_name; *p; p++) {
        int n;
        glyph_cols(*p, &n);
        w += n + 1;
    }
    if (w > 0) {
        w--;
    }
    return w;
}

static int name_scroll_width(void)
{
    int w = name_content_width() + BOARD_RGB_COUNT;
    return w < BOARD_RGB_COUNT ? BOARD_RGB_COUNT : w;
}

static uint8_t name_column(int col)
{
    if (col < 0) {
        return 0;
    }
    int at = 0;
    for (const char *p = s_name; *p; p++) {
        int n;
        const uint8_t *g = glyph_cols(*p, &n);
        if (col < at + n) {
            return g[col - at];
        }
        at += n;
        if (col == at) {
            return 0;
        }
        at++;
    }
    return 0;
}

static void led_paint_name(void)
{
    if (!s_name[0]) {
        bsp_rgb_fill(0, 0, 0);
        bsp_rgb_show();
        s_led_on = false;
        return;
    }
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        uint8_t col = name_column(s_led_scroll + i);
        if (col) {
            int bits = 0;
            for (uint8_t m = col; m; m >>= 1) {
                bits += m & 1;
            }
            /* Brightness scales with lit rows in the glyph column. */
            int level = 4 + bits * 4;
            if (level > 32) {
                level = 32;
            }
            uint8_t r = ((LED_INK >> 16) & 0xFF) * level * OS_LED_MAX / (32 * 255);
            uint8_t g = ((LED_INK >> 8) & 0xFF) * level * OS_LED_MAX / (32 * 255);
            uint8_t b = (LED_INK & 0xFF) * level * OS_LED_MAX / (32 * 255);
            bsp_rgb_set(i, r, g, b);
        } else {
            bsp_rgb_set(i, 0, 0, 0);
        }
    }
    bsp_rgb_show();
    s_led_on = true;
}

static void led_clear(void)
{
    if (!s_led_on) {
        return;
    }
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
    s_led_on = false;
}

static void led_tick(void)
{
    s_led_tick++;
    /* Advance scroll every other OS tick (~200 ms). */
    if ((s_led_tick & 1) == 0) {
        return;
    }
    s_led_scroll++;
    if (s_led_scroll >= name_scroll_width()) {
        s_led_scroll = 0;
    }
    led_paint_name();
}

static void load_settings(void)
{
    strlcpy(s_name, DEFAULT_NAME, sizeof(s_name));
    strlcpy(s_title, DEFAULT_TITLE, sizeof(s_title));
    s_font_mode = FONT_NORMAL;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_name);
    if (nvs_get_str(h, "name", s_name, &n) != ESP_OK || !s_name[0]) {
        strlcpy(s_name, DEFAULT_NAME, sizeof(s_name));
    }
    n = sizeof(s_title);
    if (nvs_get_str(h, "title", s_title, &n) != ESP_OK || !s_title[0]) {
        strlcpy(s_title, DEFAULT_TITLE, sizeof(s_title));
    }
    int32_t fm = FONT_NORMAL;
    if (nvs_get_i32(h, "font", &fm) == ESP_OK && fm >= 0 && fm < FONT_COUNT) {
        s_font_mode = (int)fm;
    }
    nvs_close(h);
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "name", s_name);
    nvs_set_str(h, "title", s_title);
    nvs_set_i32(h, "font", s_font_mode);
    nvs_commit(h);
    nvs_close(h);
}

static void update_field_hint(void)
{
    if (!s_field_hint) {
        return;
    }
    if (s_field == FIELD_NAME) {
        lv_label_set_text(s_field_hint, "> Name   Title");
    } else {
        lv_label_set_text(s_field_hint, "  Name   > Title");
    }
    if (s_hint) {
        lv_label_set_text_fmt(s_hint,
                              "U/D field  SEL edit  L/R font (%s)  CANCEL exit",
                              font_mode_names[s_font_mode]);
    }
    if (s_font_lbl) {
        lv_label_set_text_fmt(s_font_lbl, "Font: %s", font_mode_names[s_font_mode]);
    }
}

static void refresh_text(void)
{
    const lv_font_t *name_font = &sol_font_32;
    const lv_font_t *title_font = &sol_font_20;
    if (s_font_mode == FONT_COMPACT) {
        name_font = &sol_font_20;
        title_font = &sol_font_14;
    } else if (s_font_mode == FONT_HUGE) {
        name_font = &sol_font_32;
        title_font = &sol_font_20;
    }

    if (s_name_lbl) {
        lv_obj_set_style_text_font(s_name_lbl, name_font, 0);
        lv_label_set_text(s_name_lbl, s_name[0] ? s_name : "(name)");
        lv_obj_set_style_text_color(s_name_lbl,
                                    ui_col(s_name[0] ? COL_TEXT : COL_DIM), 0);
    }
    if (s_title_lbl) {
        lv_obj_set_style_text_font(s_title_lbl, title_font, 0);
        lv_label_set_text(s_title_lbl, s_title[0] ? s_title : "(title)");
        lv_obj_set_style_text_color(s_title_lbl,
                                    ui_col(s_title[0] ? COL_AQUA : COL_DIM), 0);
    }
    update_field_hint();
    s_led_scroll = 0;
    led_paint_name();
}

static void build(void)
{
    if (s_view) {
        lv_obj_delete(s_view);
    }
    s_name_lbl = s_title_lbl = s_field_hint = s_font_lbl = NULL;
    s_view = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, OS_ROOT_W, CONTENT_H);
    lv_obj_set_scrollable(s_view, false);

    bool light = ui_light();
    lv_color_t fg = light ? lv_color_black() : lv_color_white();
    lv_color_t bg = light ? lv_color_white() : lv_color_black();
    bool huge = (s_font_mode == FONT_HUGE);

    int text_x = huge ? 12 : COL_X;
    int text_w = huge ? (OS_ROOT_W - 24) : COL_W;
    int name_y = huge ? 36 : 36;
    int title_y = huge ? 96 : 84;

    if (!huge) {
        lv_obj_t *box = lv_obj_create(s_view);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, QR_BOX, QR_BOX);
        lv_obj_set_pos(box, 8, 10);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(box, bg, 0);
        lv_obj_set_style_radius(box, 8, 0);
        if (!light) {
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_color(box, ui_col(COL_BORDER), 0);
        }
        lv_obj_set_scrollable(box, false);

        lv_obj_t *qr = lv_qrcode_create(box);
        lv_qrcode_set_size(qr, QR_SIZE);
        lv_qrcode_set_dark_color(qr, fg);
        lv_qrcode_set_light_color(qr, bg);
        if (lv_qrcode_update(qr, CONTACT_PAGE_URL, strlen(CONTACT_PAGE_URL)) != LV_RESULT_OK) {
            lv_obj_delete(qr);
            lv_obj_t *err = label_at(box, &sol_font_14, light ? COL_BG : COL_TEXT,
                                     "Link too long\nfor a QR code", 0, 0, 0);
            lv_obj_center(err);
        } else {
            lv_obj_center(qr);
        }

        lv_obj_t *cap = label_at(s_view, &sol_mono_12, COL_DIM, "Scan for Contact",
                                 4, 10 + QR_BOX + 4, QR_BOX + 8);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    }

    label_at(s_view, &sol_mono_12, COL_DIM, "NAMEPLATE", text_x, huge ? 10 : 12, text_w);
    s_name_lbl = label_at(s_view, &sol_font_32, COL_TEXT, "", text_x, name_y, text_w);
    if (huge) {
        lv_obj_set_style_text_align(s_name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }
    s_title_lbl = label_at(s_view, &sol_font_20, COL_AQUA, "", text_x, title_y, text_w);
    if (huge) {
        lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    int bar_y = huge ? 130 : 118;
    ui_gradient_bar(s_view, text_x, bar_y, text_w, 3);

    label_at(s_view, &sol_mono_12, COL_DIM, "EDIT (NVS)", text_x, bar_y + 12, text_w);
    s_field_hint = label_at(s_view, &sol_mono_12, COL_TEXT, "", text_x, bar_y + 30, text_w);
    s_font_lbl = label_at(s_view, &sol_mono_12, COL_DIM, "", text_x, bar_y + 48, text_w);

    refresh_text();
    s_light = light;
}

static void open_kb(void)
{
    const char *prompt = s_field == FIELD_NAME ? "Nameplate name" : "Nameplate title";
    size_t max = s_field == FIELD_NAME ? NAME_MAX - 1 : TITLE_MAX - 1;
    s_kb = ui_kb_create(s_root, prompt, max);
}

static void cycle_font(int dir)
{
    s_font_mode = (s_font_mode + dir + FONT_COUNT) % FONT_COUNT;
    save_settings();
    build();
    os_toast("%s", font_mode_names[s_font_mode]);
}

static void on_enter(lv_obj_t *root)
{
    s_root = root;
    s_view = NULL;
    s_kb = NULL;
    s_field = FIELD_NAME;
    s_led_scroll = 0;
    s_led_tick = 0;
    s_led_on = false;
    load_settings();
    battle_paint_stage(root);
    s_hint = ui_hint(root, "");
    build();
}

static void on_exit(void)
{
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    led_clear();
    s_root = s_view = s_hint = NULL;
    s_name_lbl = s_title_lbl = s_field_hint = s_font_lbl = NULL;
}

static void on_tick(void)
{
    if (s_kb) {
        return;
    }
    if (ui_light() != s_light) {
        build();
    }
    led_tick();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (s_kb) {
        switch (ui_kb_key(s_kb, k, repeat)) {
        case UI_KB_DONE: {
            const char *t = ui_kb_text(s_kb);
            if (s_field == FIELD_NAME) {
                strlcpy(s_name, t, sizeof(s_name));
            } else {
                strlcpy(s_title, t, sizeof(s_title));
            }
            ui_kb_delete(s_kb);
            s_kb = NULL;
            save_settings();
            refresh_text();
            return true;
        }
        case UI_KB_CANCEL:
            ui_kb_delete(s_kb);
            s_kb = NULL;
            update_field_hint();
            return true;
        default:
            return true;
        }
    }

    if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
        s_field = (s_field + 1) % FIELD_COUNT;
        update_field_hint();
        return true;
    }
    if (k == OS_KEY_LEFT && !repeat) {
        cycle_font(-1);
        return true;
    }
    if (k == OS_KEY_RIGHT && !repeat) {
        cycle_font(+1);
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        open_kb();
        return true;
    }
    return false;
}

const os_app_t app_nameplate = {
    .id = "nameplate",
    .name = "Nameplate",
    .symbol = LV_SYMBOL_EDIT,
    .accent = COL_AQUA,
    .on_enter = on_enter,
    .on_exit = on_exit,
    .on_key = on_key,
    .on_tick = on_tick,
};
