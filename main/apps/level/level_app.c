// Spirit level / tilt for the BP26 badge (320×240).
//
// Hardware: stock Breakpoint ESP32-C5 has no IMU or accelerometer in the
// Solana OS BSP (board.h I2C is TCA9534 only; no accel/gyro driver). This
// app therefore uses a software tilt model driven by the D-pad — the only
// motion-like input present. SELECT zeros the reading. When pitch and roll
// are within LEVEL_EPS_DEG, the vial shows LEVEL and the WS2812 strip glows
// green. Offline; no Wi-Fi / mesh / SD.
//
// A future BSP accel API can feed level_sample() without changing the UI.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "core/os.h"
#include "core/ui.h"

/* Workshop tool look — navy vial, cream face, teal bubble, red/yellow marks. */
#define L_NAVY          0x0B1F3A
#define L_FACE          0xE8F0F8
#define L_TEAL          0x1ABC9C
#define L_TEAL_DK       0x0E7A66
#define L_RED           0xE74C3C
#define L_YELLOW        0xF1C40F
#define L_DIM           0x5A6A7A
#define L_OK            0x27AE60
#define L_RING          0x2C4A6E

#define LEVEL_EPS_DEG   0.8f
#define TILT_STEP_DEG   1.5f
#define TILT_MAX_DEG    45.0f
#define BUBBLE_R        14
#define VIAL_R          78
#define DEAD_R          10      /* centre ring radius (px) */

/* Layout on 320×218 app root. */
#define TITLE_Y         4
#define VIAL_CX         (OS_ROOT_W / 2)
#define VIAL_CY         108
#define READ_Y          (OS_ROOT_H - UI_HINT_H - 36)

typedef struct {
    float pitch_deg;    /* +nose up */
    float roll_deg;     /* +right wing down */
    bool hardware;      /* true when a real IMU/accel feeds samples */
} level_sample_t;

static lv_obj_t *s_root;
static lv_obj_t *s_vial, *s_cross_h, *s_cross_v, *s_dead, *s_bubble;
static lv_obj_t *s_pitch_lbl, *s_roll_lbl, *s_status, *s_source, *s_hint;
static float s_pitch, s_roll;           /* simulated (or last hardware) attitude */
static float s_zero_p, s_zero_r;        /* calibration offsets */
static bool s_was_level;
static bool s_led_on;

/* ---- Sensor path -------------------------------------------------------- */

/*
 * BP26 / Solana OS BSP today: no IMU, no accelerometer, no gyro API.
 * Closest BSP "sensors": bsp_battery_* (ADC) and bsp_buttons_* (digital).
 * Until a BSP accel driver exists, synthesise pitch/roll from D-pad nudges
 * held in s_pitch / s_roll.
 */
static void level_sample(level_sample_t *out)
{
    out->pitch_deg = s_pitch - s_zero_p;
    out->roll_deg = s_roll - s_zero_r;
    out->hardware = false;
}

static void clamp_tilt(float *v)
{
    if (*v > TILT_MAX_DEG) {
        *v = TILT_MAX_DEG;
    } else if (*v < -TILT_MAX_DEG) {
        *v = -TILT_MAX_DEG;
    }
}

static void nudge(float dp, float dr)
{
    s_pitch += dp;
    s_roll += dr;
    clamp_tilt(&s_pitch);
    clamp_tilt(&s_roll);
}

static void zero_cal(void)
{
    s_zero_p = s_pitch;
    s_zero_r = s_roll;
}

static void led_level(bool on)
{
    if (on == s_led_on) {
        return;
    }
    s_led_on = on;
    if (!on) {
        bsp_rgb_fill(0, 0, 0);
        bsp_rgb_show();
        return;
    }
    uint8_t r = ((L_OK >> 16) & 0xFF) * OS_LED_MAX / 255;
    uint8_t g = ((L_OK >> 8) & 0xFF) * OS_LED_MAX / 255;
    uint8_t b = (L_OK & 0xFF) * OS_LED_MAX / 255;
    bsp_rgb_fill(r, g, b);
    bsp_rgb_show();
}

/* Map degrees → bubble offset. Bubble runs opposite gravity (tool feel). */
static void bubble_xy(float pitch, float roll, int *ox, int *oy)
{
    float max_travel = (float)(VIAL_R - BUBBLE_R - 2);
    float nx = -roll / TILT_MAX_DEG;
    float ny = pitch / TILT_MAX_DEG;
    float mag = sqrtf(nx * nx + ny * ny);
    if (mag > 1.0f) {
        nx /= mag;
        ny /= mag;
    }
    *ox = (int)lroundf(nx * max_travel);
    *oy = (int)lroundf(ny * max_travel);
}

static void refresh(void)
{
    level_sample_t s;
    level_sample(&s);

    int ox, oy;
    bubble_xy(s.pitch_deg, s.roll_deg, &ox, &oy);
    if (s_bubble) {
        lv_obj_set_pos(s_bubble,
                       VIAL_CX - BUBBLE_R + ox,
                       VIAL_CY - BUBBLE_R + oy);
    }

    bool level = fabsf(s.pitch_deg) <= LEVEL_EPS_DEG
              && fabsf(s.roll_deg) <= LEVEL_EPS_DEG;

    if (s_pitch_lbl) {
        lv_label_set_text_fmt(s_pitch_lbl, "P %+5.1f°", (double)s.pitch_deg);
    }
    if (s_roll_lbl) {
        lv_label_set_text_fmt(s_roll_lbl, "R %+5.1f°", (double)s.roll_deg);
    }
    if (s_status) {
        if (level) {
            lv_label_set_text(s_status, "LEVEL");
            lv_obj_set_style_text_color(s_status, lv_color_hex(L_OK), 0);
        } else {
            lv_label_set_text(s_status, "tilt");
            lv_obj_set_style_text_color(s_status, lv_color_hex(L_RED), 0);
        }
    }
    if (s_source) {
        lv_label_set_text(s_source, s.hardware ? "IMU" : "D-pad sim");
    }
    if (s_dead) {
        lv_obj_set_style_border_color(s_dead,
                                      lv_color_hex(level ? L_OK : L_YELLOW), 0);
    }
    if (s_vial) {
        lv_obj_set_style_border_color(s_vial,
                                      lv_color_hex(level ? L_OK : L_RING), 0);
    }

    led_level(level);
    if (level && !s_was_level) {
        os_toast("Level");
    }
    s_was_level = level;
}

static void leave(void)
{
    led_level(false);
    s_root = NULL;
    s_vial = s_cross_h = s_cross_v = s_dead = s_bubble = NULL;
    s_pitch_lbl = s_roll_lbl = s_status = s_source = s_hint = NULL;
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_pitch = s_roll = 0;
    s_zero_p = s_zero_r = 0;
    s_was_level = false;
    s_led_on = false;

    lv_obj_set_style_bg_color(root, lv_color_hex(L_NAVY), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *title = ui_label(root, &sol_font_20, COL_TEXT, "Spirit Level");
    lv_obj_set_style_text_color(title, lv_color_hex(L_FACE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    /* Outer vial disc */
    s_vial = lv_obj_create(root);
    lv_obj_set_size(s_vial, VIAL_R * 2, VIAL_R * 2);
    lv_obj_set_pos(s_vial, VIAL_CX - VIAL_R, VIAL_CY - VIAL_R);
    lv_obj_set_style_bg_color(s_vial, lv_color_hex(L_FACE), 0);
    lv_obj_set_style_bg_opa(s_vial, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_vial, 3, 0);
    lv_obj_set_style_border_color(s_vial, lv_color_hex(L_RING), 0);
    lv_obj_set_style_pad_all(s_vial, 0, 0);
    lv_obj_clear_flag(s_vial, LV_OBJ_FLAG_SCROLLABLE);

    /* Crosshair */
    s_cross_h = lv_obj_create(root);
    lv_obj_set_size(s_cross_h, VIAL_R * 2 - 16, 2);
    lv_obj_set_pos(s_cross_h, VIAL_CX - VIAL_R + 8, VIAL_CY - 1);
    lv_obj_set_style_bg_color(s_cross_h, lv_color_hex(L_RING), 0);
    lv_obj_set_style_bg_opa(s_cross_h, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_cross_h, 0, 0);
    lv_obj_set_style_radius(s_cross_h, 0, 0);
    lv_obj_clear_flag(s_cross_h, LV_OBJ_FLAG_SCROLLABLE);

    s_cross_v = lv_obj_create(root);
    lv_obj_set_size(s_cross_v, 2, VIAL_R * 2 - 16);
    lv_obj_set_pos(s_cross_v, VIAL_CX - 1, VIAL_CY - VIAL_R + 8);
    lv_obj_set_style_bg_color(s_cross_v, lv_color_hex(L_RING), 0);
    lv_obj_set_style_bg_opa(s_cross_v, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_cross_v, 0, 0);
    lv_obj_set_style_radius(s_cross_v, 0, 0);
    lv_obj_clear_flag(s_cross_v, LV_OBJ_FLAG_SCROLLABLE);

    /* Dead-band ring (level target) */
    s_dead = lv_obj_create(root);
    lv_obj_set_size(s_dead, DEAD_R * 2 + 4, DEAD_R * 2 + 4);
    lv_obj_set_pos(s_dead, VIAL_CX - DEAD_R - 2, VIAL_CY - DEAD_R - 2);
    lv_obj_set_style_bg_opa(s_dead, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_dead, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dead, 2, 0);
    lv_obj_set_style_border_color(s_dead, lv_color_hex(L_YELLOW), 0);
    lv_obj_set_style_pad_all(s_dead, 0, 0);
    lv_obj_clear_flag(s_dead, LV_OBJ_FLAG_SCROLLABLE);

    /* Bubble */
    s_bubble = lv_obj_create(root);
    lv_obj_set_size(s_bubble, BUBBLE_R * 2, BUBBLE_R * 2);
    lv_obj_set_pos(s_bubble, VIAL_CX - BUBBLE_R, VIAL_CY - BUBBLE_R);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(L_TEAL), 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_bubble, 2, 0);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(L_TEAL_DK), 0);
    lv_obj_set_style_pad_all(s_bubble, 0, 0);
    lv_obj_clear_flag(s_bubble, LV_OBJ_FLAG_SCROLLABLE);

    s_pitch_lbl = ui_label(root, &sol_mono_12, COL_TEXT, "P +0.0°");
    lv_obj_set_style_text_color(s_pitch_lbl, lv_color_hex(L_FACE), 0);
    lv_obj_set_pos(s_pitch_lbl, 8, READ_Y);

    s_roll_lbl = ui_label(root, &sol_mono_12, COL_TEXT, "R +0.0°");
    lv_obj_set_style_text_color(s_roll_lbl, lv_color_hex(L_FACE), 0);
    lv_obj_align(s_roll_lbl, LV_ALIGN_TOP_RIGHT, -8, READ_Y);

    s_status = ui_label(root, &sol_font_20, COL_TEXT, "LEVEL");
    lv_obj_set_style_text_color(s_status, lv_color_hex(L_OK), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, READ_Y - 4);

    s_source = ui_label(root, &sol_mono_12, COL_DIM, "D-pad sim");
    lv_obj_set_style_text_color(s_source, lv_color_hex(L_DIM), 0);
    lv_obj_align(s_source, LV_ALIGN_TOP_MID, 0, TITLE_Y + 22);

    s_hint = ui_hint(root,
                     LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                     " tilt  SELECT zero  CANCEL exit");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(L_FACE), 0);

    refresh();
}

static void on_tick(void)
{
    /* Hardware path would re-sample here; D-pad sim only needs redraw after keys. */
    (void)0;
}

static bool on_key(os_key_t k, bool repeat)
{
    float step = repeat ? TILT_STEP_DEG * 2.0f : TILT_STEP_DEG;

    switch (k) {
    case OS_KEY_UP:
        nudge(step, 0);
        refresh();
        return true;
    case OS_KEY_DOWN:
        nudge(-step, 0);
        refresh();
        return true;
    case OS_KEY_LEFT:
        nudge(0, -step);
        refresh();
        return true;
    case OS_KEY_RIGHT:
        nudge(0, step);
        refresh();
        return true;
    case OS_KEY_SELECT:
        if (!repeat) {
            zero_cal();
            refresh();
            os_toast("Zeroed");
        }
        return true;
    case OS_KEY_BACK:
        if (!repeat) {
            return false; /* exit */
        }
        return true;
    default:
        return false;
    }
}

const os_app_t app_level = {
    .id = "level",
    .name = "Level",
    .symbol = LV_SYMBOL_GPS,
    .accent = L_TEAL,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
