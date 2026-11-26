// Spirit level / tilt for the BP26 badge (320×240).
//
// Hardware: stock Breakpoint ESP32-C5 has no IMU or accelerometer in the
// Solana OS BSP (board.h I2C is TCA9534 only; no accel/gyro driver). This
// app therefore uses a software tilt model driven by the D-pad — the only
// motion-like input present. Offline; no Wi-Fi / mesh / SD.
//
// Sensor path is isolated behind level_accel_* so a future BSP driver can
// feed real pitch/roll without rewriting the UI. On enter the app
// auto-calibrates (current attitude → zero). SELECT re-zeros.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "core/os.h"
#include "core/theme_battle.h"
#include "core/ui.h"

/* Pocket-battle workshop look — sky stage, cream vial, teal bubble. */
#define L_OK            T_TEAL
#define L_RING          T_NAVY
#define L_TEAL_DK       0x1A8A80

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
static bool s_auto_cal_done;

/* ---- Future BSP accelerometer hook -------------------------------------- */
/*
 * Plug a real driver here when the BSP gains an IMU / accel API
 * (e.g. bsp_accel_read). Today stock BP26 has none — I2C hosts only the
 * TCA9534 button expander — so these stubs always report unavailable and
 * the D-pad simulation path is used.
 *
 * Contract for a future BSP:
 *   level_accel_available() → true once the device is probed/ready
 *   level_accel_read()      → fill pitch/roll in degrees, return true on OK
 * The UI / vial path never talks to hardware directly; it only calls
 * level_sample(), which prefers the accel hook when available.
 */
static bool level_accel_available(void)
{
    /* TODO(bsp): return bsp_accel_ready() when Solana OS ships an IMU API. */
    return false;
}

static bool level_accel_read(float *pitch_deg, float *roll_deg)
{
    if (!pitch_deg || !roll_deg) {
        return false;
    }
    if (!level_accel_available()) {
        return false;
    }
    /* TODO(bsp):
     *   bsp_accel_sample_t s;
     *   if (bsp_accel_read(&s) != ESP_OK) return false;
     *   *pitch_deg = s.pitch_deg;
     *   *roll_deg  = s.roll_deg;
     *   return true;
     */
    (void)pitch_deg;
    (void)roll_deg;
    return false;
}

/* ---- Sensor path -------------------------------------------------------- */

static void level_sample(level_sample_t *out)
{
    float hp = 0, hr = 0;
    if (level_accel_read(&hp, &hr)) {
        s_pitch = hp;
        s_roll = hr;
        out->hardware = true;
    } else {
        out->hardware = false;
    }
    out->pitch_deg = s_pitch - s_zero_p;
    out->roll_deg = s_roll - s_zero_r;
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
    /* D-pad only drives the sim model; ignore when hardware is live. */
    if (level_accel_available()) {
        return;
    }
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

static void auto_calibrate(void)
{
    /* Prefer a fresh hardware sample before locking the zero. */
    float hp = 0, hr = 0;
    if (level_accel_read(&hp, &hr)) {
        s_pitch = hp;
        s_roll = hr;
    }
    zero_cal();
    s_auto_cal_done = true;
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
            lv_obj_set_style_text_color(s_status, lv_color_hex(T_RED), 0);
        }
    }
    if (s_source) {
        lv_label_set_text(s_source, s.hardware ? "IMU" : "D-pad sim");
    }
    if (s_dead) {
        lv_obj_set_style_border_color(s_dead,
                                      lv_color_hex(level ? L_OK : T_YELLOW), 0);
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
    s_auto_cal_done = false;

    battle_paint_stage(root);

    lv_obj_t *title = battle_label(root, &sol_font_20, T_NAVY, "Spirit Level");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    /* Outer vial disc */
    s_vial = lv_obj_create(root);
    lv_obj_set_size(s_vial, VIAL_R * 2, VIAL_R * 2);
    lv_obj_set_pos(s_vial, VIAL_CX - VIAL_R, VIAL_CY - VIAL_R);
    lv_obj_set_style_bg_color(s_vial, lv_color_hex(T_FACE), 0);
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
    lv_obj_set_style_border_color(s_dead, lv_color_hex(T_YELLOW), 0);
    lv_obj_set_style_pad_all(s_dead, 0, 0);
    lv_obj_clear_flag(s_dead, LV_OBJ_FLAG_SCROLLABLE);

    /* Bubble */
    s_bubble = lv_obj_create(root);
    lv_obj_set_size(s_bubble, BUBBLE_R * 2, BUBBLE_R * 2);
    lv_obj_set_pos(s_bubble, VIAL_CX - BUBBLE_R, VIAL_CY - BUBBLE_R);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(T_TEAL), 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_bubble, 2, 0);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(L_TEAL_DK), 0);
    lv_obj_set_style_pad_all(s_bubble, 0, 0);
    lv_obj_clear_flag(s_bubble, LV_OBJ_FLAG_SCROLLABLE);

    s_pitch_lbl = battle_label(root, &sol_mono_12, T_NAVY, "P +0.0°");
    lv_obj_set_pos(s_pitch_lbl, 8, READ_Y);

    s_roll_lbl = battle_label(root, &sol_mono_12, T_NAVY, "R +0.0°");
    lv_obj_align(s_roll_lbl, LV_ALIGN_TOP_RIGHT, -8, READ_Y);

    s_status = battle_label(root, &sol_font_20, L_OK, "LEVEL");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, READ_Y - 4);

    s_source = battle_label(root, &sol_mono_12, T_DIM, "D-pad sim");
    lv_obj_align(s_source, LV_ALIGN_TOP_MID, 0, TITLE_Y + 22);

    s_hint = battle_hint(root,
                         LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                         " tilt  SELECT zero  CANCEL exit");

    /* Auto-calibrate: treat the attitude at open as level. */
    auto_calibrate();
    refresh();
    os_toast("Auto-calibrated");
}

static void on_tick(void)
{
    if (!s_root) {
        return;
    }
    /* Hardware path re-samples every tick; D-pad sim only needs key-driven redraw. */
    if (level_accel_available()) {
        if (!s_auto_cal_done) {
            auto_calibrate();
            os_toast("Auto-calibrated");
        }
        refresh();
    }
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
            s_auto_cal_done = true;
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
    .accent = T_TEAL,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
