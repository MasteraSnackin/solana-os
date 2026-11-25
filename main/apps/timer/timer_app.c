// Timer: stopwatch and countdown for the BP26 badge (320×240).
// Two tabs — Stopwatch (mm:ss.cc) and Timer (set MM:SS, alert at zero).
// Alert: screen flash + WS2812 blink. Offline; no Wi-Fi / mesh / SD.
//
// Visual theme: pocket-battle inspired (bright primaries, thick playful
// panels). Original palette — no copyrighted creature art or names.

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "bsp.h"
#include "core/os.h"
#include "core/ui.h"

enum { TAB_SW = 0, TAB_CD, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = { "Stopwatch", "Timer" };

enum { FIELD_MIN = 0, FIELD_SEC };

#define ALERT_TICKS     50      /* ~5 s of flash/blink at OS_TICK_MS */
#define MAX_MM          99
#define MAX_SS          59

/* Pocket-battle palette (original hexes; not remapped by ui_col light map). */
#define T_SKY           0x5EC8F0
#define T_NAVY          0x1A2A6C
#define T_CREAM         0xFFF8E0
#define T_INK           0x1A1A2E
#define T_RED           0xE63946
#define T_YELLOW        0xFFD60A
#define T_BLUE          0x3A86FF
#define T_TEAL          0x2EC4B6
#define T_ORANGE        0xFF9F1C
#define T_WHITE         0xFFFFFF
#define T_SOFT          0xE8F7FF
#define T_DIM           0x4A5568
#define T_FACE          0xFFFDF5

/* Layout for OS_ROOT 320×218 (status bar sits above). */
#define TAB_BAR_H       28
#define CLOCK_Y         (TAB_BAR_H + 8)
#define CLOCK_H         84
#define FIELD_W         86
#define FIELD_H         54
#define BTN_Y           (OS_ROOT_H - UI_HINT_H - 48)
#define BTN_H           40
#define BTN_W           148
#define STRIPE_H        8

static lv_obj_t *s_root, *s_tabs;
static lv_obj_t *s_tab_lbl[TAB_COUNT];
static lv_obj_t *s_clock;               /* outer navy frame */
static lv_obj_t *s_face;                /* cream clock face */
static lv_obj_t *s_stripe;              /* yellow top stripe on clock */
static lv_obj_t *s_time;                /* single readout (stopwatch / run) */
static lv_obj_t *s_mm_box, *s_ss_box;   /* countdown field boxes */
static lv_obj_t *s_mm_lbl, *s_ss_lbl, *s_colon;
static lv_obj_t *s_sub_chip, *s_sub;
static lv_obj_t *s_btn_a, *s_btn_b;     /* SELECT / CANCEL soft buttons */
static lv_obj_t *s_btn_a_lbl, *s_btn_b_lbl;
static lv_obj_t *s_btn_a_key, *s_btn_b_key;
static lv_obj_t *s_hint_bar, *s_hint, *s_flash;
static int s_tab;

/* Stopwatch */
static bool s_sw_run;
static int64_t s_sw_acc_us;     /* accumulated while paused */
static int64_t s_sw_t0_us;      /* esp_timer when last started */

/* Countdown */
static bool s_cd_run;
static bool s_cd_alert;
static int s_cd_field;          /* FIELD_MIN or FIELD_SEC while editing */
static int s_cd_set_mm, s_cd_set_ss;
static int64_t s_cd_left_us;    /* remaining when paused / at start */
static int64_t s_cd_t0_us;
static int s_alert_left;        /* on_tick counts left in alert */
static int s_alert_phase;       /* blink phase */

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void led_off(void)
{
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
}

static void led_fill_hex(uint32_t col)
{
    uint8_t r = ((col >> 16) & 0xFF) * OS_LED_MAX / 255;
    uint8_t g = ((col >> 8) & 0xFF) * OS_LED_MAX / 255;
    uint8_t b = (col & 0xFF) * OS_LED_MAX / 255;
    bsp_rgb_fill(r, g, b);
    bsp_rgb_show();
}

static void led_alert(bool on)
{
    if (!on) {
        led_off();
        return;
    }
    /* Coarser phase so each lit blink can swap red ↔ yellow. */
    int blink = s_alert_phase / 2;
    led_fill_hex((blink / 2) % 2 == 0 ? T_RED : T_YELLOW);
}

static void flash_screen(bool on)
{
    if (!s_flash) {
        return;
    }
    lv_obj_set_style_bg_opa(s_flash, on ? LV_OPA_50 : LV_OPA_TRANSP, 0);
    int blink = s_alert_phase / 2;
    lv_obj_set_style_bg_color(s_flash,
                              lv_color_hex((blink / 2) % 2 == 0 ? T_YELLOW : T_RED),
                              0);
}

static void stop_alert(void)
{
    s_cd_alert = false;
    s_alert_left = 0;
    s_alert_phase = 0;
    flash_screen(false);
    led_off();
}

static int64_t sw_elapsed_us(void)
{
    int64_t e = s_sw_acc_us;
    if (s_sw_run) {
        e += now_us() - s_sw_t0_us;
    }
    if (e < 0) {
        e = 0;
    }
    /* Cap display at 99:59.99 */
    const int64_t cap = (int64_t)(MAX_MM * 60 + MAX_SS) * 1000000LL + 990000LL;
    return e > cap ? cap : e;
}

static int64_t cd_left_us(void)
{
    if (s_cd_alert) {
        return 0;
    }
    int64_t left = s_cd_left_us;
    if (s_cd_run) {
        left = s_cd_left_us - (now_us() - s_cd_t0_us);
    }
    return left > 0 ? left : 0;
}

static void fmt_sw(char *buf, size_t n, int64_t us)
{
    int cs = (int)(us / 10000LL);       /* centiseconds */
    int mm = cs / 6000;
    int ss = (cs / 100) % 60;
    int cc = cs % 100;
    snprintf(buf, n, "%02d:%02d.%02d", mm, ss, cc);
}

static void fmt_cd(char *buf, size_t n, int64_t us)
{
    /* Ceil while time remains so the last second reads 00:01 until zero. */
    int sec = 0;
    if (us > 0) {
        sec = (int)((us + 999999LL) / 1000000LL);
    }
    int mm = sec / 60;
    int ss = sec % 60;
    if (mm > MAX_MM) {
        mm = MAX_MM;
        ss = MAX_SS;
    }
    snprintf(buf, n, "%02d:%02d", mm, ss);
}

static bool cd_editing(void)
{
    if (s_tab != TAB_CD || s_cd_run || s_cd_alert) {
        return false;
    }
    return s_cd_left_us == (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL;
}

static void style_soft_btn(lv_obj_t *btn, lv_obj_t *lbl, lv_obj_t *key,
                           const char *label, const char *keycap,
                           uint32_t fill, uint32_t ink, bool filled)
{
    if (!btn || !lbl) {
        return;
    }
    uint32_t bg = filled ? fill : T_SOFT;
    uint32_t fg = filled ? ink : T_NAVY;
    uint32_t border = filled ? T_NAVY : fill;
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(border), 0);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
    if (key) {
        lv_label_set_text(key, keycap);
        lv_obj_set_style_text_color(key, lv_color_hex(filled ? ink : T_DIM), 0);
        lv_obj_set_style_text_opa(key, filled ? LV_OPA_70 : LV_OPA_COVER, 0);
    }
}

static void paint_field(lv_obj_t *box, lv_obj_t *lbl, bool focused)
{
    if (!box || !lbl) {
        return;
    }
    lv_obj_set_style_bg_color(box, lv_color_hex(focused ? T_YELLOW : T_SOFT), 0);
    lv_obj_set_style_border_width(box, focused ? 3 : 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(focused ? T_RED : T_NAVY), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(focused ? T_INK : T_NAVY), 0);
}

static void paint_tabs(void)
{
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *l = s_tab_lbl[i];
        if (!l) {
            continue;
        }
        bool on = (i == s_tab);
        lv_obj_t *pill = lv_obj_get_parent(l);
        if (!pill) {
            continue;
        }
        uint32_t fill = on ? (i == TAB_SW ? T_YELLOW : T_TEAL) : T_SOFT;
        uint32_t ink = on ? T_INK : T_DIM;
        lv_obj_set_style_bg_color(pill, lv_color_hex(fill), 0);
        lv_obj_set_style_border_width(pill, on ? 3 : 2, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(T_NAVY), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
    }
}

static void update_actions(void)
{
    const char *a = "Start";
    const char *b = "Exit";
    uint32_t fill = T_RED;
    uint32_t ink = T_WHITE;
    bool filled = true;

    if (s_tab == TAB_SW) {
        if (s_sw_run) {
            a = "Stop";
            b = "Reset";
            fill = T_ORANGE;
            ink = T_INK;
        } else if (s_sw_acc_us > 0) {
            a = "Start";
            b = "Reset";
        } else {
            a = "Start";
            b = "Exit";
        }
    } else if (s_cd_alert) {
        a = "Dismiss";
        b = "Dismiss";
        fill = T_YELLOW;
        ink = T_INK;
    } else if (s_cd_run) {
        a = "Pause";
        b = "Reset";
        fill = T_ORANGE;
        ink = T_INK;
    } else if (s_cd_left_us > 0 &&
               s_cd_left_us != (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL) {
        a = "Resume";
        b = "Reset";
    } else if (s_cd_set_mm == 0 && s_cd_set_ss == 0) {
        a = "Start";
        b = "Exit";
        filled = false; /* nothing set yet — Start looks inactive */
    } else {
        a = "Start";
        b = "Reset";
    }

    style_soft_btn(s_btn_a, s_btn_a_lbl, s_btn_a_key, a, "SELECT", fill, ink, filled);
    style_soft_btn(s_btn_b, s_btn_b_lbl, s_btn_b_key, b, "CANCEL", T_BLUE, T_WHITE, false);
}

static void update_hint(void)
{
    if (!s_hint) {
        return;
    }
    if (s_tab == TAB_SW) {
        if (s_sw_run) {
            lv_label_set_text(s_hint, "SELECT stop   CANCEL reset");
        } else if (s_sw_acc_us > 0) {
            lv_label_set_text(s_hint, "SELECT start   CANCEL reset   "
                              LV_SYMBOL_LEFT "/" LV_SYMBOL_RIGHT " mode");
        } else {
            lv_label_set_text(s_hint, "SELECT start   "
                              LV_SYMBOL_LEFT "/" LV_SYMBOL_RIGHT " mode   CANCEL exit");
        }
        return;
    }
    if (s_cd_alert) {
        lv_label_set_text(s_hint, "SELECT or CANCEL dismiss alert");
        return;
    }
    if (s_cd_run) {
        lv_label_set_text(s_hint, "SELECT pause   CANCEL reset");
        return;
    }
    if (s_cd_left_us > 0 &&
        s_cd_left_us != (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL) {
        lv_label_set_text(s_hint, "SELECT resume   UP/DOWN set   CANCEL reset");
        return;
    }
    if (s_cd_field == FIELD_MIN) {
        lv_label_set_text(s_hint, "UP/DOWN minutes   RIGHT seconds   "
                          LV_SYMBOL_LEFT " Stopwatch");
    } else {
        lv_label_set_text(s_hint, "UP/DOWN seconds   LEFT minutes   "
                          "SELECT start");
    }
}

static void update_sub(void)
{
    if (!s_sub || !s_sub_chip) {
        return;
    }
    uint32_t chip = T_SOFT;
    uint32_t ink = T_DIM;

    if (s_tab == TAB_SW) {
        if (s_sw_run) {
            lv_label_set_text(s_sub, "Running");
            chip = T_YELLOW;
            ink = T_INK;
        } else if (s_sw_acc_us > 0) {
            lv_label_set_text(s_sub, "Paused");
            chip = T_ORANGE;
            ink = T_INK;
        } else {
            lv_label_set_text(s_sub, "Ready");
            chip = T_SOFT;
            ink = T_NAVY;
        }
    } else if (s_cd_alert) {
        lv_label_set_text(s_sub, "Time's up!");
        chip = T_RED;
        ink = T_WHITE;
    } else if (s_cd_run) {
        lv_label_set_text(s_sub, "Counting down");
        chip = T_TEAL;
        ink = T_INK;
    } else if (s_cd_left_us > 0 &&
               s_cd_left_us != (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL) {
        lv_label_set_text(s_sub, "Paused");
        chip = T_ORANGE;
        ink = T_INK;
    } else if (s_cd_field == FIELD_MIN) {
        lv_label_set_text(s_sub, "Minutes  " LV_SYMBOL_UP LV_SYMBOL_DOWN);
        chip = T_YELLOW;
        ink = T_INK;
    } else {
        lv_label_set_text(s_sub, "Seconds  " LV_SYMBOL_UP LV_SYMBOL_DOWN);
        chip = T_YELLOW;
        ink = T_INK;
    }

    lv_obj_set_style_bg_color(s_sub_chip, lv_color_hex(chip), 0);
    lv_obj_set_style_border_color(s_sub_chip, lv_color_hex(T_NAVY), 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(ink), 0);
}

static void show_edit_fields(bool edit)
{
    if (s_time) {
        if (edit) {
            lv_obj_add_flag(s_time, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_time, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_t *parts[] = { s_mm_box, s_ss_box, s_colon };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (!parts[i]) {
            continue;
        }
        if (edit) {
            lv_obj_clear_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_time(void)
{
    if (!s_time) {
        return;
    }
    char buf[16];
    bool edit = cd_editing();
    show_edit_fields(edit);
    paint_tabs();

    if (s_tab == TAB_SW) {
        fmt_sw(buf, sizeof(buf), sw_elapsed_us());
        lv_label_set_text(s_time, buf);
        lv_obj_set_style_text_color(s_time, lv_color_hex(T_INK), 0);
        if (s_stripe) {
            lv_obj_set_style_bg_color(s_stripe, lv_color_hex(s_sw_run ? T_YELLOW : T_RED), 0);
        }
    } else if (edit) {
        snprintf(buf, sizeof(buf), "%02d", s_cd_set_mm);
        lv_label_set_text(s_mm_lbl, buf);
        snprintf(buf, sizeof(buf), "%02d", s_cd_set_ss);
        lv_label_set_text(s_ss_lbl, buf);
        paint_field(s_mm_box, s_mm_lbl, s_cd_field == FIELD_MIN);
        paint_field(s_ss_box, s_ss_lbl, s_cd_field == FIELD_SEC);
        if (s_stripe) {
            lv_obj_set_style_bg_color(s_stripe, lv_color_hex(T_TEAL), 0);
        }
    } else {
        fmt_cd(buf, sizeof(buf), cd_left_us());
        lv_label_set_text(s_time, buf);
        lv_obj_set_style_text_color(s_time,
                                    lv_color_hex(s_cd_alert ? T_RED : T_INK), 0);
        if (s_stripe) {
            lv_obj_set_style_bg_color(s_stripe,
                                      lv_color_hex(s_cd_alert ? T_YELLOW :
                                                   (s_cd_run ? T_TEAL : T_BLUE)),
                                      0);
        }
    }
    update_sub();
    update_hint();
    update_actions();
}

static void sw_reset(void)
{
    s_sw_run = false;
    s_sw_acc_us = 0;
    s_sw_t0_us = 0;
}

static void sw_toggle(void)
{
    if (s_sw_run) {
        s_sw_acc_us += now_us() - s_sw_t0_us;
        s_sw_run = false;
    } else {
        s_sw_t0_us = now_us();
        s_sw_run = true;
    }
}

static void cd_apply_setpoint(void)
{
    int total = s_cd_set_mm * 60 + s_cd_set_ss;
    s_cd_left_us = (int64_t)total * 1000000LL;
}

static void cd_reset(void)
{
    stop_alert();
    s_cd_run = false;
    cd_apply_setpoint();
}

static void cd_start_alert(void)
{
    s_cd_run = false;
    s_cd_left_us = 0;
    s_cd_alert = true;
    s_alert_left = ALERT_TICKS;
    s_alert_phase = 0;
    flash_screen(true);
    led_alert(true);
    os_toast("Time's up!");
}

static void cd_toggle(void)
{
    if (s_cd_alert) {
        stop_alert();
        cd_apply_setpoint();
        return;
    }
    if (s_cd_run) {
        s_cd_left_us = cd_left_us();
        s_cd_run = false;
        return;
    }
    if (s_cd_left_us <= 0) {
        cd_apply_setpoint();
    }
    if (s_cd_left_us <= 0) {
        os_toast("Set a time first");
        return;
    }
    s_cd_t0_us = now_us();
    s_cd_run = true;
}

static void cd_adjust(int dir)
{
    if (s_cd_run || s_cd_alert) {
        return;
    }
    /* Editing always works on setpoint; if paused mid-run, treat as re-edit */
    if (s_cd_left_us != (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL) {
        /* snap display back to setpoint for editing */
        s_cd_left_us = (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL;
    }
    if (s_cd_field == FIELD_MIN) {
        s_cd_set_mm += dir;
        if (s_cd_set_mm < 0) {
            s_cd_set_mm = MAX_MM;
        } else if (s_cd_set_mm > MAX_MM) {
            s_cd_set_mm = 0;
        }
    } else {
        s_cd_set_ss += dir;
        if (s_cd_set_ss < 0) {
            s_cd_set_ss = MAX_SS;
        } else if (s_cd_set_ss > MAX_SS) {
            s_cd_set_ss = 0;
        }
    }
    cd_apply_setpoint();
}

static void show_tab(int tab)
{
    if (tab == s_tab) {
        update_time();
        return;
    }
    /* Leaving a tab pauses that mode; clear alert */
    if (s_tab == TAB_SW && s_sw_run) {
        s_sw_acc_us += now_us() - s_sw_t0_us;
        s_sw_run = false;
    }
    if (s_tab == TAB_CD) {
        if (s_cd_run) {
            s_cd_left_us = cd_left_us();
            s_cd_run = false;
        }
        stop_alert();
    }
    s_tab = tab;
    update_time();
}

static lv_obj_t *make_soft_btn(lv_obj_t *root, int x, lv_obj_t **lbl_out, lv_obj_t **key_out)
{
    lv_obj_t *b = lv_obj_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, BTN_Y);
    lv_obj_set_size(b, BTN_W, BTN_H);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_scrollable(b, false);

    lv_obj_t *lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, &sol_font_20, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *key = lv_label_create(b);
    lv_obj_set_style_text_font(key, &sol_mono_12, 0);
    lv_obj_align(key, LV_ALIGN_BOTTOM_MID, 0, -2);

    *lbl_out = lbl;
    *key_out = key;
    return b;
}

static lv_obj_t *make_field_box(lv_obj_t *parent, int x, int y, lv_obj_t **lbl_out)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, FIELD_W, FIELD_H);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(T_SOFT), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(T_NAVY), 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_scrollable(box, false);

    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_font(lbl, &sol_font_32, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(T_NAVY), 0);
    lv_label_set_text(lbl, "00");
    lv_obj_center(lbl);
    *lbl_out = lbl;
    return box;
}

static void make_playful_tabs(lv_obj_t *root)
{
    s_tabs = lv_obj_create(root);
    lv_obj_remove_style_all(s_tabs);
    lv_obj_set_size(s_tabs, OS_ROOT_W, TAB_BAR_H);
    lv_obj_set_pos(s_tabs, 0, 0);
    lv_obj_set_style_bg_opa(s_tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_tabs, lv_color_hex(T_NAVY), 0);
    lv_obj_set_scrollable(s_tabs, false);

    int pill_w = (OS_ROOT_W - 20) / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *pill = lv_obj_create(s_tabs);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, pill_w - 4, TAB_BAR_H - 6);
        lv_obj_set_pos(pill, 8 + i * pill_w, 3);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, 8, 0);
        lv_obj_set_style_border_width(pill, 2, 0);
        lv_obj_set_scrollable(pill, false);

        lv_obj_t *l = lv_label_create(pill);
        lv_obj_set_style_text_font(l, &sol_font_14, 0);
        lv_label_set_text(l, tab_names[i]);
        lv_obj_center(l);
        s_tab_lbl[i] = l;
    }
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_tab = TAB_SW;
    s_sw_run = false;
    s_sw_acc_us = 0;
    s_sw_t0_us = 0;
    s_cd_run = false;
    s_cd_alert = false;
    s_cd_field = FIELD_MIN;
    s_cd_set_mm = 0;
    s_cd_set_ss = 0;
    s_cd_left_us = 0;
    s_cd_t0_us = 0;
    s_alert_left = 0;
    s_alert_phase = 0;

    /* Sky stage behind everything. */
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(T_SKY), 0);

    make_playful_tabs(root);

    /* Thick navy frame + cream face + coloured header stripe. */
    s_clock = lv_obj_create(root);
    lv_obj_remove_style_all(s_clock);
    lv_obj_set_pos(s_clock, 8, CLOCK_Y);
    lv_obj_set_size(s_clock, OS_ROOT_W - 16, CLOCK_H);
    lv_obj_set_style_bg_opa(s_clock, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_clock, lv_color_hex(T_NAVY), 0);
    lv_obj_set_style_radius(s_clock, 12, 0);
    lv_obj_set_scrollable(s_clock, false);

    s_face = lv_obj_create(s_clock);
    lv_obj_remove_style_all(s_face);
    lv_obj_set_pos(s_face, 4, 4);
    lv_obj_set_size(s_face, OS_ROOT_W - 24, CLOCK_H - 8);
    lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_face, lv_color_hex(T_FACE), 0);
    lv_obj_set_style_radius(s_face, 8, 0);
    lv_obj_set_scrollable(s_face, false);

    s_stripe = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_stripe);
    lv_obj_set_pos(s_stripe, 0, 0);
    lv_obj_set_size(s_stripe, OS_ROOT_W - 24, STRIPE_H);
    lv_obj_set_style_bg_opa(s_stripe, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_stripe, lv_color_hex(T_RED), 0);
    lv_obj_set_style_radius(s_stripe, 0, 0);
    lv_obj_set_scrollable(s_stripe, false);

    /* Single large readout — stopwatch and running/paused countdown. */
    s_time = lv_label_create(s_face);
    lv_obj_set_style_text_font(s_time, &sol_font_32, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(T_INK), 0);
    lv_label_set_text(s_time, "00:00.00");
    lv_obj_set_width(s_time, OS_ROOT_W - 40);
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 4);

    /* Countdown edit: two focused fields with a colon between. */
    int fields_w = FIELD_W * 2 + 20;
    int fx = ((OS_ROOT_W - 24) - fields_w) / 2;
    int fy = (CLOCK_H - 8 - FIELD_H) / 2 + 2;
    s_mm_box = make_field_box(s_face, fx, fy, &s_mm_lbl);
    s_colon = lv_label_create(s_face);
    lv_obj_set_style_text_font(s_colon, &sol_font_32, 0);
    lv_obj_set_style_text_color(s_colon, lv_color_hex(T_NAVY), 0);
    lv_label_set_text(s_colon, ":");
    lv_obj_align(s_colon, LV_ALIGN_CENTER, 0, 2);
    s_ss_box = make_field_box(s_face, fx + FIELD_W + 20, fy, &s_ss_lbl);

    /* Status chip under the clock. */
    s_sub_chip = lv_obj_create(root);
    lv_obj_remove_style_all(s_sub_chip);
    lv_obj_set_size(s_sub_chip, 160, 22);
    lv_obj_align(s_sub_chip, LV_ALIGN_TOP_MID, 0, CLOCK_Y + CLOCK_H + 6);
    lv_obj_set_style_bg_opa(s_sub_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_sub_chip, lv_color_hex(T_SOFT), 0);
    lv_obj_set_style_border_width(s_sub_chip, 2, 0);
    lv_obj_set_style_border_color(s_sub_chip, lv_color_hex(T_NAVY), 0);
    lv_obj_set_style_radius(s_sub_chip, 11, 0);
    lv_obj_set_scrollable(s_sub_chip, false);

    s_sub = lv_label_create(s_sub_chip);
    lv_obj_set_style_text_font(s_sub, &sol_font_14, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(T_NAVY), 0);
    lv_label_set_text(s_sub, "");
    lv_obj_center(s_sub);

    s_btn_a = make_soft_btn(root, 8, &s_btn_a_lbl, &s_btn_a_key);
    s_btn_b = make_soft_btn(root, OS_ROOT_W - 8 - BTN_W, &s_btn_b_lbl, &s_btn_b_key);

    /* Full-root flash overlay for countdown alert (above content, below hint). */
    s_flash = lv_obj_create(root);
    lv_obj_remove_style_all(s_flash);
    lv_obj_set_size(s_flash, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_pos(s_flash, 0, 0);
    lv_obj_set_style_bg_color(s_flash, lv_color_hex(T_YELLOW), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_flash, LV_OBJ_FLAG_CLICKABLE);

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
    lv_label_set_text(s_hint, "");
    lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 6, 0);

    led_off();
    show_tab(TAB_SW);
}

static void leave(void)
{
    stop_alert();
    led_off();
    s_root = NULL;
    s_tabs = NULL;
    s_tab_lbl[0] = s_tab_lbl[1] = NULL;
    s_clock = NULL;
    s_face = NULL;
    s_stripe = NULL;
    s_time = NULL;
    s_mm_box = s_ss_box = NULL;
    s_mm_lbl = s_ss_lbl = s_colon = NULL;
    s_sub_chip = NULL;
    s_sub = NULL;
    s_btn_a = s_btn_b = NULL;
    s_btn_a_lbl = s_btn_b_lbl = NULL;
    s_btn_a_key = s_btn_b_key = NULL;
    s_hint_bar = NULL;
    s_hint = NULL;
    s_flash = NULL;
    s_sw_run = false;
    s_cd_run = false;
}

static void on_tick(void)
{
    if (s_tab == TAB_SW && s_sw_run) {
        update_time();
        return;
    }
    if (s_tab == TAB_CD) {
        if (s_cd_run) {
            if (cd_left_us() <= 0) {
                cd_start_alert();
            }
            update_time();
            return;
        }
        if (s_cd_alert) {
            s_alert_phase++;
            bool on = (s_alert_phase / 2) % 2 == 0;
            flash_screen(on);
            led_alert(on);
            if (s_alert_left > 0) {
                s_alert_left--;
                if (s_alert_left == 0) {
                    stop_alert();
                    cd_apply_setpoint();
                }
            }
            update_time();
        }
    }
}

static bool on_key(os_key_t k, bool repeat)
{
    /* Mode tabs: LEFT/RIGHT when not mid-edit on Timer field navigation.
     * On Timer while stopped, LEFT/RIGHT first move between MM/SS; from the
     * edge they switch tabs. While running / alerting, LEFT/RIGHT switch tabs. */
    if (k == OS_KEY_LEFT || k == OS_KEY_RIGHT) {
        if (repeat) {
            return true;
        }
        if (s_tab == TAB_CD && !s_cd_run && !s_cd_alert) {
            if (k == OS_KEY_LEFT) {
                if (s_cd_field == FIELD_SEC) {
                    s_cd_field = FIELD_MIN;
                    update_time();
                    return true;
                }
                show_tab(TAB_SW);
                return true;
            }
            /* RIGHT */
            if (s_cd_field == FIELD_MIN) {
                s_cd_field = FIELD_SEC;
                update_time();
                return true;
            }
            /* already on seconds — stay (only two tabs; RIGHT from Timer is noop) */
            return true;
        }
        if (s_tab == TAB_SW && k == OS_KEY_RIGHT) {
            show_tab(TAB_CD);
            return true;
        }
        if (s_tab == TAB_CD && k == OS_KEY_LEFT) {
            show_tab(TAB_SW);
            return true;
        }
        return true;
    }

    if (s_tab == TAB_SW) {
        if (k == OS_KEY_SELECT && !repeat) {
            sw_toggle();
            update_time();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            if (s_sw_run || s_sw_acc_us > 0) {
                sw_reset();
                update_time();
                return true;
            }
            return false; /* exit */
        }
        return true;
    }

    /* Timer tab */
    if (s_cd_alert) {
        if ((k == OS_KEY_SELECT || k == OS_KEY_BACK) && !repeat) {
            stop_alert();
            cd_apply_setpoint();
            update_time();
            return true;
        }
        return true;
    }

    if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
        cd_adjust(k == OS_KEY_UP ? 1 : -1);
        update_time();
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        cd_toggle();
        update_time();
        return true;
    }
    if (k == OS_KEY_BACK && !repeat) {
        if (s_cd_run) {
            cd_reset();
            update_time();
            return true;
        }
        if (s_cd_left_us > 0 || s_cd_set_mm || s_cd_set_ss) {
            s_cd_set_mm = 0;
            s_cd_set_ss = 0;
            cd_reset();
            update_time();
            return true;
        }
        return false; /* exit */
    }
    return true;
}

const os_app_t app_timer = {
    .id = "timer",
    .name = "Timer",
    .symbol = LV_SYMBOL_REFRESH,
    .accent = T_YELLOW,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
