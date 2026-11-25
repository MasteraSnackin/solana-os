// Timer: stopwatch and countdown for the BP26 badge.
// Two tabs — Stopwatch (mm:ss.cc) and Timer (set MM:SS, alert at zero).
// Alert: screen flash + WS2812 blink. Offline; no Wi-Fi / mesh / SD.

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

static lv_obj_t *s_root, *s_tabs, *s_time, *s_sub, *s_hint, *s_flash;
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

static void led_alert(bool on)
{
    if (!on) {
        led_off();
        return;
    }
    uint8_t r = ((COL_ORANGE >> 16) & 0xFF) * OS_LED_MAX / 255;
    uint8_t g = ((COL_ORANGE >> 8) & 0xFF) * OS_LED_MAX / 255;
    uint8_t b = (COL_ORANGE & 0xFF) * OS_LED_MAX / 255;
    bsp_rgb_fill(r, g, b);
    bsp_rgb_show();
}

static void flash_screen(bool on)
{
    if (!s_flash) {
        return;
    }
    lv_obj_set_style_bg_opa(s_flash, on ? LV_OPA_60 : LV_OPA_TRANSP, 0);
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
                              LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " mode");
        } else {
            lv_label_set_text(s_hint, "SELECT start   "
                              LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " mode   CANCEL exit");
        }
        return;
    }
    /* Timer tab */
    if (s_cd_alert) {
        lv_label_set_text(s_hint, "SELECT / CANCEL dismiss");
        return;
    }
    if (s_cd_run) {
        lv_label_set_text(s_hint, "SELECT pause   CANCEL reset");
        return;
    }
    if (s_cd_left_us > 0 && s_cd_left_us != (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL) {
        lv_label_set_text(s_hint, "SELECT resume   UP/DOWN set   CANCEL reset");
        return;
    }
    if (s_cd_set_mm == 0 && s_cd_set_ss == 0) {
        lv_label_set_text(s_hint, "UP/DOWN set   LEFT/RIGHT field   CANCEL exit");
    } else {
        lv_label_set_text(s_hint, "SELECT start   UP/DOWN set   "
                          LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " field/mode");
    }
}

static void update_sub(void)
{
    if (!s_sub) {
        return;
    }
    if (s_tab == TAB_SW) {
        lv_label_set_text(s_sub, s_sw_run ? "Running" : (s_sw_acc_us > 0 ? "Paused" : "Ready"));
        lv_obj_set_style_text_color(s_sub, ui_col(s_sw_run ? COL_GREEN : COL_DIM), 0);
        return;
    }
    if (s_cd_alert) {
        lv_label_set_text(s_sub, "Time's up!");
        lv_obj_set_style_text_color(s_sub, ui_col(COL_ORANGE), 0);
        return;
    }
    if (s_cd_run) {
        lv_label_set_text(s_sub, "Counting down");
        lv_obj_set_style_text_color(s_sub, ui_col(COL_GREEN), 0);
        return;
    }
    if (s_cd_left_us > 0 &&
        s_cd_left_us != (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL) {
        lv_label_set_text(s_sub, "Paused");
        lv_obj_set_style_text_color(s_sub, ui_col(COL_DIM), 0);
        return;
    }
    /* Editing setpoint */
    if (s_cd_field == FIELD_MIN) {
        lv_label_set_text(s_sub, "Set minutes  " LV_SYMBOL_UP LV_SYMBOL_DOWN);
    } else {
        lv_label_set_text(s_sub, "Set seconds  " LV_SYMBOL_UP LV_SYMBOL_DOWN);
    }
    lv_obj_set_style_text_color(s_sub, ui_col(COL_AQUA), 0);
}

static void update_time(void)
{
    if (!s_time) {
        return;
    }
    char buf[16];
    if (s_tab == TAB_SW) {
        fmt_sw(buf, sizeof(buf), sw_elapsed_us());
        lv_label_set_text(s_time, buf);
        lv_obj_set_style_text_color(s_time, ui_col(COL_TEXT), 0);
    } else {
        if (!s_cd_run && !s_cd_alert &&
            s_cd_left_us == (int64_t)(s_cd_set_mm * 60 + s_cd_set_ss) * 1000000LL) {
            /* Show setpoint with field highlight via colour */
            snprintf(buf, sizeof(buf), "%02d:%02d", s_cd_set_mm, s_cd_set_ss);
            lv_label_set_text(s_time, buf);
            lv_obj_set_style_text_color(s_time,
                                        ui_col(s_cd_field == FIELD_MIN ? COL_AQUA : COL_TEXT), 0);
        } else {
            fmt_cd(buf, sizeof(buf), cd_left_us());
            lv_label_set_text(s_time, buf);
            lv_obj_set_style_text_color(s_time, ui_col(s_cd_alert ? COL_ORANGE : COL_TEXT), 0);
        }
    }
    update_sub();
    update_hint();
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
        ui_tabs_select(s_tabs, tab);
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
    ui_tabs_select(s_tabs, tab);
    update_time();
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

    s_tabs = ui_tabs(root, tab_names, TAB_COUNT);

    s_time = ui_label(root, &sol_font_32, COL_TEXT, "00:00.00");
    lv_obj_set_width(s_time, OS_ROOT_W - 16);
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, UI_TABS_H + 48);

    s_sub = ui_label(root, &sol_font_14, COL_DIM, "");
    lv_obj_set_width(s_sub, OS_ROOT_W - 16);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, UI_TABS_H + 100);

    /* Full-root flash overlay for countdown alert (above content, below hint). */
    s_flash = lv_obj_create(root);
    lv_obj_remove_style_all(s_flash);
    lv_obj_set_size(s_flash, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_pos(s_flash, 0, 0);
    lv_obj_set_style_bg_color(s_flash, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_flash, LV_OBJ_FLAG_CLICKABLE);

    s_hint = ui_hint(root, "");
    led_off();
    show_tab(TAB_SW);
}

static void leave(void)
{
    stop_alert();
    led_off();
    s_root = NULL;
    s_tabs = NULL;
    s_time = NULL;
    s_sub = NULL;
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
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
