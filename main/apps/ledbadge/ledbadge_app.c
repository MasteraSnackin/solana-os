// LED Badge: wearable patterns on the BP26's 8x WS2812B strip (GPIO27).
// Pick a pattern, brightness and colour on device; optionally set a name that
// shows on the LCD and LEDs. Name text has two layouts: Scroll (LCD marquee +
// LED column sweep) or Full display (static LCD name + entire message fitted
// across the 8 LEDs). An esp_timer keeps the strip animating after you leave
// the app (turn Pattern to Off to stop). Brightness is capped at OS_LED_MAX.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "nvs.h"
#include "bsp.h"
#include "core/os.h"
#include "core/ui.h"

#define NVS_NS          "ledbadge"
#define LEDBADGE_NAME_MAX 24  /* avoid clash with POSIX NAME_MAX */
#define FRAME_MS        40
#define PREVIEW_Y       4
#define PREVIEW_H       28
#define LIST_Y          (PREVIEW_Y + PREVIEW_H + 4)

enum {
    MODE_OFF,
    MODE_SOLID,
    MODE_GRADIENT,
    MODE_CHASE,
    MODE_BREATHE,
    MODE_COMET,
    MODE_SPARKLE,
    MODE_NAME,
    MODE_COUNT
};

enum { ROW_PATTERN, ROW_BRIGHT, ROW_COLOR, ROW_SPEED, ROW_DISPLAY, ROW_NAME, ROW_COUNT };

enum { SPEED_SLOW, SPEED_NORMAL, SPEED_FAST, SPEED_COUNT };

// How Name mode paints the message on the strip / LCD preview.
enum { NAME_DISP_SCROLL, NAME_DISP_FULL, NAME_DISP_COUNT };

static const char *const mode_names[MODE_COUNT] = {
    "Off", "Solid", "Solana gradient", "Rainbow chase",
    "Breathe", "Comet", "Sparkle", "Name scroll",
};

static const char *const speed_names[SPEED_COUNT] = { "Slow", "Normal", "Fast" };
static const int speed_div[SPEED_COUNT] = { 3, 2, 1 };

static const char *const name_disp_names[NAME_DISP_COUNT] = {
    "Scroll", "Full display",
};

static const uint32_t palette[] = {
    COL_PURPLE, COL_GREEN, COL_PINK, COL_BLUE, COL_AQUA, COL_LIME, COL_YELLOW, COL_ORANGE, 0xFFFFFF,
};
static const char *const palette_names[] = {
    "Purple", "Green", "Pink", "Blue", "Aqua", "Lime", "Yellow", "Orange", "White",
};
#define PALETTE_N (int)(sizeof(palette) / sizeof(palette[0]))

// 5x7 glyphs for A-Z, 0-9, space and a few punctuation marks. Each column is
// a bit0=top .. bit6=bottom byte; the strip shows one column at a time across
// LEDs 0..6 (letter height along the strip).
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

static int s_mode = MODE_GRADIENT;
static int s_bright = OS_LED_MAX;       // 1..OS_LED_MAX
static int s_color;                     // palette index
static int s_speed = SPEED_NORMAL;
static int s_name_disp = NAME_DISP_SCROLL;
static char s_name[LEDBADGE_NAME_MAX] = "SOLANA";
static int s_tick;
static int s_scroll;                    // column offset for name scroll mode
static uint32_t s_rand;

static lv_obj_t *s_root, *s_preview[BOARD_RGB_COUNT], *s_hint, *s_marquee;
static ui_list_t *s_list;
static ui_kb_t *s_kb;
static esp_timer_handle_t s_timer;
static bool s_ui_live;                  // preview widgets exist

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t, int n)
{
    if (n <= 0) {
        return a;
    }
    int r = ((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t) / n;
    int g = ((a >> 8) & 0xFF) + (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t) / n;
    int bl = (a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * t) / n;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static uint32_t hue(int h)
{
    h %= 360;
    if (h < 0) {
        h += 360;
    }
    int x = (h % 60) * 255 / 60;
    switch (h / 60) {
    case 0:  return 0xFF0000 | (x << 8);
    case 1:  return ((255 - x) << 16) | 0xFF00;
    case 2:  return 0x00FF00 | x;
    case 3:  return ((255 - x) << 8) | 0xFF;
    case 4:  return (x << 16) | 0xFF;
    default: return 0xFF0000 | (255 - x);
    }
}

static uint32_t next_rand(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand;
}

static void led_scaled(int i, uint32_t rgb)
{
    uint8_t r = ((rgb >> 16) & 0xFF) * s_bright / 255;
    uint8_t g = ((rgb >> 8) & 0xFF) * s_bright / 255;
    uint8_t b = (rgb & 0xFF) * s_bright / 255;
    bsp_rgb_set(i, r, g, b);
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

// Pixel columns of the message itself (glyphs + 1-column gaps), no scroll pad.
static int name_content_width(void)
{
    int w = 0;
    for (const char *p = s_name; *p; p++) {
        int n;
        glyph_cols(*p, &n);
        w += n + 1;
    }
    if (w > 0) {
        w--;                            // drop trailing gap after last glyph
    }
    return w;
}

// Total pixel columns in the scrolling message (content + pad to clear strip).
static int name_width(void)
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
            return 0;                   // gap between letters
        }
        at++;
    }
    return 0;
}

// Map LED index 0..BOARD_RGB_COUNT-1 onto a font column for Full display:
// short messages are centered; longer ones are scaled to span the whole strip.
static uint8_t name_column_full(int led)
{
    int w = name_content_width();
    if (w <= 0 || led < 0 || led >= BOARD_RGB_COUNT) {
        return 0;
    }
    if (w <= BOARD_RGB_COUNT) {
        int pad = (BOARD_RGB_COUNT - w) / 2;
        return name_column(led - pad);
    }
    // Sample across the full message so every LED shows a slice of the text.
    int col = (led * w) / BOARD_RGB_COUNT;
    if (col >= w) {
        col = w - 1;
    }
    return name_column(col);
}

static void paint_name_led(int i, uint8_t col, uint32_t solid)
{
    if (col) {
        int bits = 0;
        for (uint8_t m = col; m; m >>= 1) {
            bits += m & 1;
        }
        uint32_t c = lerp_rgb(0, solid, 4 + bits * 4, 32);
        led_scaled(i, c);
    } else {
        bsp_rgb_set(i, 0, 0, 0);
    }
}

static void apply_frame(void)
{
    int t = s_tick;
    uint32_t solid = palette[s_color];
    switch (s_mode) {
    case MODE_OFF:
        bsp_rgb_fill(0, 0, 0);
        break;
    case MODE_SOLID:
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            led_scaled(i, solid);
        }
        break;
    case MODE_GRADIENT: {
        int phase = (t * 4) % 512;
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            int u = (i * 32 + phase) % 512;
            led_scaled(i, lerp_rgb(COL_PURPLE, COL_GREEN, u < 256 ? u : 511 - u, 255));
        }
        break;
    }
    case MODE_CHASE:
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            led_scaled(i, hue(i * 45 + t * 12));
        }
        break;
    case MODE_BREATHE: {
        // Triangle 0..255..0 over ~2.5 s at normal speed.
        int period = 64;
        int u = t % period;
        int level = u < period / 2 ? u * 2 : (period - u) * 2;
        uint32_t c = lerp_rgb(0x000000, solid, level, 255);
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            led_scaled(i, c);
        }
        break;
    }
    case MODE_COMET: {
        int head = t % (BOARD_RGB_COUNT * 2);
        if (head >= BOARD_RGB_COUNT) {
            head = BOARD_RGB_COUNT * 2 - 1 - head;
        }
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            int d = abs(i - head);
            uint32_t c = d == 0 ? solid : d == 1 ? lerp_rgb(solid, 0, 1, 2) : d == 2 ? lerp_rgb(solid, 0, 3, 4) : 0;
            led_scaled(i, c);
        }
        break;
    }
    case MODE_SPARKLE:
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            uint32_t r = next_rand();
            if ((r & 0xFF) < 40) {
                led_scaled(i, (r & 1) ? solid : hue((int)(r >> 8)));
            } else {
                // Dim trail of previous frame.
                uint32_t cur = bsp_rgb_get(i);
                uint8_t rr = ((cur >> 16) & 0xFF) * 2 / 3;
                uint8_t gg = ((cur >> 8) & 0xFF) * 2 / 3;
                uint8_t bb = (cur & 0xFF) * 2 / 3;
                bsp_rgb_set(i, rr, gg, bb);
            }
        }
        break;
    case MODE_NAME:
        // Scroll: sliding window over font columns. Full: fit whole message.
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            uint8_t col = (s_name_disp == NAME_DISP_FULL)
                ? name_column_full(i)
                : name_column(s_scroll + i);
            paint_name_led(i, col, solid);
        }
        break;
    }
    bsp_rgb_show();
}

static void preview_sync(void)
{
    if (!s_ui_live) {
        return;
    }
    bool name_ui = s_mode == MODE_NAME && s_name[0];
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        lv_obj_set_hidden(s_preview[i], name_ui);
        if (name_ui) {
            continue;
        }
        uint32_t c = bsp_rgb_get(i);
        // Expand the dim OS_LED_MAX values so the on-screen dots stay visible.
        int r = ((c >> 16) & 0xFF) * 255 / (s_bright > 0 ? s_bright : 1);
        int g = ((c >> 8) & 0xFF) * 255 / (s_bright > 0 ? s_bright : 1);
        int b = (c & 0xFF) * 255 / (s_bright > 0 ? s_bright : 1);
        if (r > 255) {
            r = 255;
        }
        if (g > 255) {
            g = 255;
        }
        if (b > 255) {
            b = 255;
        }
        if (c == 0) {
            lv_obj_set_style_bg_color(s_preview[i], ui_col(COL_PANEL), 0);
        } else {
            lv_obj_set_style_bg_color(s_preview[i], lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b), 0);
        }
    }
    if (s_marquee) {
        if (name_ui) {
            lv_label_set_text(s_marquee, s_name);
            if (s_name_disp == NAME_DISP_FULL) {
                // Static full-preview render: large centered name, no marquee.
                lv_obj_set_style_text_font(s_marquee, &sol_font_32, 0);
                lv_obj_update_layout(s_marquee);
                int w = (int)lv_obj_get_width(s_marquee);
                int h = (int)lv_obj_get_height(s_marquee);
                if (w < 8) {
                    w = (int)strlen(s_name) * 16;
                }
                if (h < 8) {
                    h = 28;
                }
                int x = (OS_ROOT_W - w) / 2;
                if (x < 0) {
                    x = 0;
                }
                int y = PREVIEW_Y + (PREVIEW_H - h) / 2;
                if (y < PREVIEW_Y) {
                    y = PREVIEW_Y;
                }
                lv_obj_set_pos(s_marquee, x, y);
            } else {
                lv_obj_set_style_text_font(s_marquee, &sol_font_20, 0);
                int w = (int)lv_obj_get_width(s_marquee);
                if (w < 8) {
                    w = (int)strlen(s_name) * 10;
                }
                int x = OS_ROOT_W - ((s_tick * 3) % (OS_ROOT_W + w));
                lv_obj_set_pos(s_marquee, x, PREVIEW_Y + 2);
            }
            lv_obj_set_hidden(s_marquee, false);
        } else {
            lv_obj_set_hidden(s_marquee, true);
        }
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    if (s_mode == MODE_OFF) {
        return;
    }
    if (s_tick % speed_div[s_speed] == 0) {
        if (s_mode == MODE_NAME && s_name_disp == NAME_DISP_SCROLL) {
            s_scroll++;
            if (s_scroll >= name_width()) {
                s_scroll = 0;
            }
        }
        apply_frame();
    }
    s_tick++;
    esp_timer_start_once(s_timer, FRAME_MS * 1000);
}

static void timer_ensure(void)
{
    if (!s_timer) {
        const esp_timer_create_args_t args = { .callback = timer_cb, .name = "ledbadge" };
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_timer);
    if (s_mode == MODE_OFF) {
        bsp_rgb_fill(0, 0, 0);
        bsp_rgb_show();
        return;
    }
    apply_frame();
    esp_timer_start_once(s_timer, FRAME_MS * 1000);
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, "mode", s_mode);
    nvs_set_i32(h, "bright", s_bright);
    nvs_set_i32(h, "color", s_color);
    nvs_set_i32(h, "speed", s_speed);
    nvs_set_i32(h, "namedisp", s_name_disp);
    nvs_set_str(h, "name", s_name);
    nvs_commit(h);
    nvs_close(h);
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v;
    if (nvs_get_i32(h, "mode", &v) == ESP_OK && v >= 0 && v < MODE_COUNT) {
        s_mode = (int)v;
    }
    if (nvs_get_i32(h, "bright", &v) == ESP_OK && v >= 1 && v <= OS_LED_MAX) {
        s_bright = (int)v;
    }
    if (nvs_get_i32(h, "color", &v) == ESP_OK && v >= 0 && v < PALETTE_N) {
        s_color = (int)v;
    }
    if (nvs_get_i32(h, "speed", &v) == ESP_OK && v >= 0 && v < SPEED_COUNT) {
        s_speed = (int)v;
    }
    if (nvs_get_i32(h, "namedisp", &v) == ESP_OK && v >= 0 && v < NAME_DISP_COUNT) {
        s_name_disp = (int)v;
    }
    size_t n = sizeof(s_name);
    nvs_get_str(h, "name", s_name, &n);
    nvs_close(h);
}

static void row(int i, ui_row_t *r, void *ctx)
{
    (void)ctx;
    switch (i) {
    case ROW_PATTERN:
        strlcpy(r->title, "Pattern", sizeof(r->title));
        strlcpy(r->sub, "SELECT cycles; runs after you leave", sizeof(r->sub));
        strlcpy(r->right, mode_names[s_mode], sizeof(r->right));
        r->right_color = s_mode == MODE_OFF ? COL_DIM : COL_GREEN;
        break;
    case ROW_BRIGHT: {
        strlcpy(r->title, "Brightness", sizeof(r->title));
        strlcpy(r->sub, LV_SYMBOL_LEFT " dimmer  " LV_SYMBOL_RIGHT " brighter", sizeof(r->sub));
        char bar[OS_LED_MAX + 1];
        for (int b = 0; b < OS_LED_MAX; b++) {
            bar[b] = b < s_bright ? '#' : '-';
        }
        bar[OS_LED_MAX] = 0;
        strlcpy(r->right, bar, sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    }
    case ROW_COLOR:
        strlcpy(r->title, "Colour", sizeof(r->title));
        strlcpy(r->sub, "Solid, breathe, comet, sparkle, name", sizeof(r->sub));
        strlcpy(r->right, palette_names[s_color], sizeof(r->right));
        r->right_color = palette[s_color];
        break;
    case ROW_SPEED:
        strlcpy(r->title, "Speed", sizeof(r->title));
        strlcpy(r->sub, "Animation and name scroll rate", sizeof(r->sub));
        strlcpy(r->right, speed_names[s_speed], sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    case ROW_DISPLAY:
        strlcpy(r->title, "Display", sizeof(r->title));
        strlcpy(r->sub, "Name: scroll/marquee or full strip", sizeof(r->sub));
        strlcpy(r->right, name_disp_names[s_name_disp], sizeof(r->right));
        r->right_color = s_name_disp == NAME_DISP_FULL ? COL_GREEN : COL_TEXT;
        break;
    case ROW_NAME:
        strlcpy(r->title, "Name", sizeof(r->title));
        if (s_name_disp == NAME_DISP_FULL) {
            strlcpy(r->sub, "Full display: fit text across LEDs", sizeof(r->sub));
        } else {
            strlcpy(r->sub, "Scroll: LCD marquee + LED columns", sizeof(r->sub));
        }
        strlcpy(r->right, s_name[0] ? s_name : "(set)", sizeof(r->right));
        r->right_color = s_name[0] ? COL_TEXT : COL_DIM;
        break;
    }
}

static void hint_for_row(void)
{
    int i = ui_list_selected(s_list);
    if (i == ROW_BRIGHT) {
        lv_label_set_text(s_hint, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  "
                          LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " brightness  CANCEL exit");
    } else if (i == ROW_NAME) {
        lv_label_set_text(s_hint, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT type name  CANCEL exit");
    } else {
        lv_label_set_text(s_hint, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT change  CANCEL exit");
    }
}

static void build_preview(lv_obj_t *root)
{
    int gap = 4;
    int dot = (OS_ROOT_W - 16 - gap * (BOARD_RGB_COUNT - 1)) / BOARD_RGB_COUNT;
    if (dot > 24) {
        dot = 24;
    }
    int total = BOARD_RGB_COUNT * dot + (BOARD_RGB_COUNT - 1) * gap;
    int x0 = (OS_ROOT_W - total) / 2;
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(root);
        lv_obj_remove_style_all(d);
        lv_obj_set_pos(d, x0 + i * (dot + gap), PREVIEW_Y + (PREVIEW_H - dot) / 2);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, ui_col(COL_PANEL), 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, ui_col(COL_BORDER), 0);
        s_preview[i] = d;
    }
    s_marquee = ui_label(root, &sol_font_20, COL_TEXT, "");
    lv_obj_set_pos(s_marquee, 0, PREVIEW_Y);
    lv_obj_set_hidden(s_marquee, true);
}

static void open_name_kb(void)
{
    s_kb = ui_kb_create(s_root, "Badge name (A-Z, 0-9)", LEDBADGE_NAME_MAX - 1);
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_kb = NULL;
    s_ui_live = true;
    s_rand = 0xC0FFEEu ^ (uint32_t)s_tick;
    load_settings();
    build_preview(root);
    s_list = ui_list_create(root, LIST_Y, OS_ROOT_H - LIST_Y - UI_HINT_H, row, NULL);
    ui_list_set_count(s_list, ROW_COUNT);
    s_hint = ui_hint(root, "");
    hint_for_row();
    timer_ensure();
    preview_sync();
}

static void on_leave(void)
{
    s_ui_live = false;
    s_root = NULL;
    s_list = NULL;
    s_hint = NULL;
    s_marquee = NULL;
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    save_settings();
    // Timer keeps running when a pattern is active so the badge stays lit.
    if (s_mode == MODE_OFF) {
        if (s_timer) {
            esp_timer_stop(s_timer);
        }
        bsp_rgb_fill(0, 0, 0);
        bsp_rgb_show();
    }
}

static void cycle_mode(int dir)
{
    s_mode = (s_mode + dir + MODE_COUNT) % MODE_COUNT;
    s_tick = 0;
    s_scroll = 0;
    timer_ensure();
    preview_sync();
    ui_list_refresh(s_list);
    save_settings();
}

static void set_bright(int next)
{
    if (next < 1 || next > OS_LED_MAX) {
        return;
    }
    s_bright = next;
    apply_frame();
    preview_sync();
    ui_list_refresh(s_list);
    save_settings();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (s_kb) {
        switch (ui_kb_key(s_kb, k, repeat)) {
        case UI_KB_DONE:
            strlcpy(s_name, ui_kb_text(s_kb), sizeof(s_name));
            ui_kb_delete(s_kb);
            s_kb = NULL;
            s_scroll = 0;
            if (s_mode != MODE_NAME && s_name[0]) {
                s_mode = MODE_NAME;
                timer_ensure();
            } else {
                apply_frame();
            }
            preview_sync();
            ui_list_refresh(s_list);
            save_settings();
            hint_for_row();
            return true;
        case UI_KB_CANCEL:
            ui_kb_delete(s_kb);
            s_kb = NULL;
            hint_for_row();
            return true;
        default:
            return true;
        }
    }

    int prev = ui_list_selected(s_list);
    if (ui_list_key(s_list, k, repeat)) {
        if (ui_list_selected(s_list) != prev) {
            hint_for_row();
        }
        return true;
    }

    int i = ui_list_selected(s_list);
    if (i == ROW_BRIGHT && (k == OS_KEY_LEFT || k == OS_KEY_RIGHT)) {
        set_bright(s_bright + (k == OS_KEY_RIGHT ? 1 : -1));
        return true;
    }
    if (k == OS_KEY_LEFT || k == OS_KEY_RIGHT) {
        int dir = k == OS_KEY_RIGHT ? 1 : -1;
        switch (i) {
        case ROW_PATTERN:
            cycle_mode(dir);
            return true;
        case ROW_COLOR:
            s_color = (s_color + dir + PALETTE_N) % PALETTE_N;
            apply_frame();
            preview_sync();
            ui_list_refresh(s_list);
            save_settings();
            return true;
        case ROW_SPEED:
            s_speed = (s_speed + dir + SPEED_COUNT) % SPEED_COUNT;
            ui_list_refresh(s_list);
            save_settings();
            return true;
        case ROW_DISPLAY:
            s_name_disp = (s_name_disp + dir + NAME_DISP_COUNT) % NAME_DISP_COUNT;
            s_scroll = 0;
            apply_frame();
            preview_sync();
            ui_list_refresh(s_list);
            save_settings();
            return true;
        default:
            break;
        }
    }
    if (k != OS_KEY_SELECT || repeat) {
        return k != OS_KEY_BACK;
    }
    switch (i) {
    case ROW_PATTERN:
        cycle_mode(1);
        break;
    case ROW_BRIGHT:
        set_bright(s_bright >= OS_LED_MAX ? 1 : s_bright + 1);
        break;
    case ROW_COLOR:
        s_color = (s_color + 1) % PALETTE_N;
        apply_frame();
        preview_sync();
        ui_list_refresh(s_list);
        save_settings();
        break;
    case ROW_SPEED:
        s_speed = (s_speed + 1) % SPEED_COUNT;
        ui_list_refresh(s_list);
        save_settings();
        break;
    case ROW_DISPLAY:
        s_name_disp = (s_name_disp + 1) % NAME_DISP_COUNT;
        s_scroll = 0;
        apply_frame();
        preview_sync();
        ui_list_refresh(s_list);
        save_settings();
        break;
    case ROW_NAME:
        open_name_kb();
        break;
    }
    return true;
}

static void on_tick(void)
{
    if (s_ui_live) {
        preview_sync();
    }
}

const os_app_t app_ledbadge = {
    .id = "ledbadge",
    .name = "LED Badge",
    .symbol = LV_SYMBOL_EYE_OPEN,
    .accent = COL_PURPLE,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
