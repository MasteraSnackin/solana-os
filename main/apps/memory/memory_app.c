// Memory: Simon-style four-pad recall for the BP26 badge.
// Watch the pattern (pads + all 8 WS2812 LEDs), then repeat with the D-pad.
// Offline — LEDs only; no Wi-Fi, mesh, or SD. Best confirmed-block run in NVS.
//
// Visual theme: shared pocket-battle palette (red / yellow / blue / teal cream
// tiles). Original colours only — no copyrighted creature art or names.

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"

#include "bsp.h"
#include "core/os.h"
#include "core/theme_battle.h"
#include "core/ui.h"

#define NVS_NS          "memory"
#define NVS_BEST        "best"

#define PAD_N           4
#define SEQ_MAX         64

#define HDR_H           30
#define PAD_GAP         4
#define PAD_W           86
#define PAD_H           46

typedef enum {
    ST_IDLE = 0,
    ST_SHOW_ON,
    ST_SHOW_GAP,
    ST_INPUT,
    ST_FLASH_OK,
    ST_OVER,
} state_t;

typedef struct {
    uint32_t color;
    const char *name;
    os_key_t key;
    int x;
    int y;
} pad_t;

// Diamond matching the D-pad: UP / LEFT / RIGHT / DOWN — battle primaries.
static const pad_t k_pads[PAD_N] = {
    { T_RED,    "UP",    OS_KEY_UP,    (OS_ROOT_W - PAD_W) / 2,               HDR_H + 8 },
    { T_YELLOW, "LEFT",  OS_KEY_LEFT,  (OS_ROOT_W / 2) - PAD_W - PAD_GAP / 2, HDR_H + 8 + PAD_H + PAD_GAP },
    { T_BLUE,   "RIGHT", OS_KEY_RIGHT, (OS_ROOT_W / 2) + PAD_GAP / 2,         HDR_H + 8 + PAD_H + PAD_GAP },
    { T_TEAL,   "DOWN",  OS_KEY_DOWN,  (OS_ROOT_W - PAD_W) / 2,               HDR_H + 8 + 2 * (PAD_H + PAD_GAP) },
};

static lv_obj_t *s_root, *s_title, *s_meta, *s_status, *s_hint;
static lv_obj_t *s_pad[PAD_N], *s_pad_lab[PAD_N], *s_stripe[PAD_N];
static state_t s_state;
static uint8_t s_seq[SEQ_MAX];
static int s_len;               /* pattern length this round */
static int s_show_i;            /* next index to flash in SHOW */
static int s_input_i;           /* next expected input index */
static int s_blocks;            /* confirmed full repeats this run */
static int32_t s_best;
static int s_lit;               /* -1 = none, else pad index */
static int s_wait;              /* remaining on_tick counts in current phase */
static int s_flash_pad;         /* pad briefly lit on correct press / OK */

// ---- NVS ------------------------------------------------------------------

static void best_load(void)
{
    s_best = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, NVS_BEST, &v) == ESP_OK && v > 0) {
        s_best = v;
    }
    nvs_close(h);
}

static void best_save(void)
{
    if (s_blocks <= s_best) {
        return;
    }
    s_best = s_blocks;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_BEST, s_best);
    nvs_commit(h);
    nvs_close(h);
}

// ---- LEDs / pads ----------------------------------------------------------

static void led_off(void)
{
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
}

static void led_pad(int pi)
{
    if (pi < 0 || pi >= PAD_N) {
        led_off();
        return;
    }
    uint32_t c = k_pads[pi].color;
    uint8_t r = ((c >> 16) & 0xFF) * OS_LED_MAX / 255;
    uint8_t g = ((c >> 8) & 0xFF) * OS_LED_MAX / 255;
    uint8_t b = (c & 0xFF) * OS_LED_MAX / 255;
    bsp_rgb_fill(r, g, b);
    bsp_rgb_show();
}

static void paint_pads(void)
{
    for (int i = 0; i < PAD_N; i++) {
        bool on = (s_lit == i) || (s_flash_pad == i);
        uint32_t fill = on ? k_pads[i].color : T_FACE;
        uint32_t border = on ? T_NAVY : k_pads[i].color;
        uint32_t ink = on ? T_INK : T_NAVY;
        lv_obj_set_style_bg_color(s_pad[i], lv_color_hex(fill), 0);
        lv_obj_set_style_border_color(s_pad[i], lv_color_hex(border), 0);
        lv_obj_set_style_border_width(s_pad[i], on ? 3 : 2, 0);
        if (s_stripe[i]) {
            lv_obj_set_style_bg_color(s_stripe[i],
                                      lv_color_hex(on ? T_NAVY : k_pads[i].color), 0);
        }
        if (s_pad_lab[i]) {
            lv_obj_set_style_text_color(s_pad_lab[i], lv_color_hex(ink), 0);
        }
    }
}

static void set_lit(int pi)
{
    s_lit = pi;
    if (pi < 0) {
        led_off();
    } else {
        led_pad(pi);
    }
    paint_pads();
}

// Timing: OS_TICK_MS = 100. Faster each confirmed block.
static int on_ticks(void)
{
    int t = 6 - s_blocks / 2;
    return t < 2 ? 2 : t;
}

static int gap_ticks(void)
{
    int t = 3 - s_blocks / 4;
    return t < 1 ? 1 : t;
}

static void set_status(const char *msg)
{
    if (s_status) {
        lv_label_set_text(s_status, msg);
    }
}

static void refresh_meta(void)
{
    if (!s_meta) {
        return;
    }
    lv_label_set_text_fmt(s_meta, "blocks %d   best %ld", s_blocks, (long)s_best);
}

// ---- game flow ------------------------------------------------------------

static void append_step(void)
{
    if (s_len >= SEQ_MAX) {
        return;
    }
    s_seq[s_len++] = (uint8_t)(esp_random() % PAD_N);
}

static void begin_show(void)
{
    s_state = ST_SHOW_ON;
    s_show_i = 0;
    s_input_i = 0;
    s_flash_pad = -1;
    set_lit(-1);
    s_wait = 2;     /* brief pause before first pad */
    set_status("Watch…");
    battle_set_hint(s_hint, "Watch the pattern");
}

static void start_game(void)
{
    s_len = 0;
    s_blocks = 0;
    s_flash_pad = -1;
    append_step();
    refresh_meta();
    begin_show();
}

static void game_over(void)
{
    s_state = ST_OVER;
    s_flash_pad = -1;
    set_lit(-1);
    best_save();
    refresh_meta();
    set_status("Game over");
    battle_set_hint(s_hint, "SELECT again  CANCEL exit");
    os_toast("Game over — %d blocks", s_blocks);
}

static void confirm_block(void)
{
    s_blocks++;
    best_save();
    refresh_meta();
    s_state = ST_FLASH_OK;
    s_wait = 4;
    s_flash_pad = -1;
    set_lit(-1);
    /* brief yellow strip flash for confirmation */
    uint8_t r = ((T_YELLOW >> 16) & 0xFF) * OS_LED_MAX / 255;
    uint8_t g = ((T_YELLOW >> 8) & 0xFF) * OS_LED_MAX / 255;
    uint8_t b = (T_YELLOW & 0xFF) * OS_LED_MAX / 255;
    bsp_rgb_fill(r, g, b);
    bsp_rgb_show();
    set_status("Nice!");
}

static void next_round(void)
{
    append_step();
    begin_show();
}

static int pad_for_key(os_key_t k)
{
    for (int i = 0; i < PAD_N; i++) {
        if (k_pads[i].key == k) {
            return i;
        }
    }
    return -1;
}

static void handle_input(int pi)
{
    if (pi < 0 || s_state != ST_INPUT) {
        return;
    }
    if (pi != (int)s_seq[s_input_i]) {
        /* wrong: flash the pressed pad briefly via lit, then over */
        set_lit(pi);
        game_over();
        return;
    }

    s_flash_pad = pi;
    set_lit(pi);
    s_input_i++;

    if (s_input_i >= s_len) {
        confirm_block();
        return;
    }

    /* short lit feedback, then wait for next press (cleared on tick) */
    s_wait = 2;
}

// ---- lifecycle ------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_state = ST_IDLE;
    s_len = 0;
    s_blocks = 0;
    s_lit = -1;
    s_flash_pad = -1;
    s_wait = 0;

    best_load();
    battle_paint_stage(root);

    s_title = battle_label(root, &sol_font_20, T_NAVY, "Memory");
    lv_obj_set_pos(s_title, 10, 4);

    s_meta = battle_label(root, &sol_mono_12, T_DIM, "");
    lv_obj_set_pos(s_meta, 10, 26);
    lv_obj_set_width(s_meta, OS_ROOT_W - 20);
    refresh_meta();

    for (int i = 0; i < PAD_N; i++) {
        s_pad[i] = battle_card(root, k_pads[i].x, k_pads[i].y, PAD_W, PAD_H,
                               k_pads[i].color);
        /* battle_card already made a stripe as first child */
        s_stripe[i] = lv_obj_get_child(s_pad[i], 0);
        s_pad_lab[i] = battle_label(s_pad[i], &sol_mono_12, T_NAVY, k_pads[i].name);
        lv_obj_center(s_pad_lab[i]);
    }
    paint_pads();

    int status_y = HDR_H + 8 + 3 * (PAD_H + PAD_GAP) + 4;
    s_status = battle_label(root, &sol_font_14, T_INK, "SELECT to start");
    lv_obj_set_pos(s_status, 10, status_y);
    lv_obj_set_width(s_status, OS_ROOT_W - 20);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    s_hint = battle_hint(root, "SELECT start  D-pad play  CANCEL exit");
    led_off();
}

static void leave(void)
{
    led_off();
    s_root = NULL;
    s_title = NULL;
    s_meta = NULL;
    s_status = NULL;
    s_hint = NULL;
    for (int i = 0; i < PAD_N; i++) {
        s_pad[i] = NULL;
        s_pad_lab[i] = NULL;
        s_stripe[i] = NULL;
    }
}

static void on_tick(void)
{
    if (s_wait > 0) {
        s_wait--;
        if (s_wait > 0) {
            return;
        }
    }

    switch (s_state) {
    case ST_SHOW_ON:
        if (s_show_i >= s_len) {
            set_lit(-1);
            s_state = ST_INPUT;
            s_input_i = 0;
            s_flash_pad = -1;
            set_status("Your turn");
            battle_set_hint(s_hint, "UP LEFT RIGHT DOWN");
            return;
        }
        set_lit(s_seq[s_show_i]);
        s_state = ST_SHOW_GAP;
        s_wait = on_ticks();
        break;

    case ST_SHOW_GAP:
        set_lit(-1);
        s_show_i++;
        s_state = ST_SHOW_ON;
        s_wait = gap_ticks();
        break;

    case ST_INPUT:
        /* clear press feedback */
        if (s_flash_pad >= 0 || s_lit >= 0) {
            s_flash_pad = -1;
            set_lit(-1);
        }
        break;

    case ST_FLASH_OK:
        led_off();
        next_round();
        break;

    default:
        break;
    }
}

static bool on_key(os_key_t k, bool repeat)
{
    if (repeat) {
        return true;
    }

    if (s_state == ST_IDLE || s_state == ST_OVER) {
        if (k == OS_KEY_SELECT) {
            start_game();
            return true;
        }
        if (k == OS_KEY_BACK) {
            return false; /* exit */
        }
        return true;
    }

    if (s_state == ST_SHOW_ON || s_state == ST_SHOW_GAP || s_state == ST_FLASH_OK) {
        /* ignore input while showing / confirming */
        if (k == OS_KEY_BACK) {
            s_state = ST_IDLE;
            s_flash_pad = -1;
            set_lit(-1);
            refresh_meta();
            set_status("SELECT to start");
            battle_set_hint(s_hint, "SELECT start  D-pad play  CANCEL exit");
            return true;
        }
        return true;
    }

    if (s_state == ST_INPUT) {
        if (k == OS_KEY_BACK) {
            s_state = ST_IDLE;
            s_flash_pad = -1;
            set_lit(-1);
            best_save();
            refresh_meta();
            set_status("SELECT to start");
            battle_set_hint(s_hint, "SELECT start  D-pad play  CANCEL exit");
            return true;
        }
        int pi = pad_for_key(k);
        if (pi >= 0) {
            handle_input(pi);
        }
        return true;
    }

    return false;
}

const os_app_t app_memory = {
    .id = "memory",
    .name = "Memory",
    .symbol = LV_SYMBOL_LIST,
    .accent = T_RED,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
