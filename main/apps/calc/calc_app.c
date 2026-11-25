// Calculator: four-function (+ − × ÷) pad for the BP26 ILI9341 (320×240).
// D-pad moves the highlight, SELECT presses a key, CANCEL backspaces (or
// exits when the entry is already clear), HOME leaves via the OS.
//
// Look: bright primary red / yellow / blue, rounded panel chrome, high-
// contrast digits. Original art only — no licensed sprites or names.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/os.h"
#include "core/ui.h"

#define ENTRY_MAX       14
#define DISP_MAX        16

// Toy-handheld primaries (true colours — bypass light/dark remapping).
#define TOY_RED         0xE3350D
#define TOY_YELLOW      0xFFCB05
#define TOY_BLUE        0x3B4CCA
#define TOY_FACE        0x1A2B6B    /* deep blue faceplate */
#define TOY_CREAM       0xFFF8DC    /* readout field */
#define TOY_INK         0x141428    /* near-black digits */
#define TOY_KEY         0xFFF4E0    /* digit key face */
#define TOY_HINT        0xA8C4FF    /* soft blue hint text */

// 320×218 app root (status bar is outside): roomy display, even 4×5 pad.
#define DISP_X          4
#define DISP_Y          2
#define DISP_H          50
#define DISP_RADIUS     12
#define GRID_X          5
#define GRID_Y          (DISP_H + 6)
#define COLS            4
#define ROWS            5
#define KEY_GAP         3
#define KEY_RADIUS      10
#define KEY_W           ((OS_ROOT_W - 2 * GRID_X - (COLS - 1) * KEY_GAP) / COLS)
#define KEY_H           ((OS_ROOT_H - GRID_Y - UI_HINT_H - 2 - (ROWS - 1) * KEY_GAP) / ROWS)

typedef enum {
    K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
    K_DOT, K_PLUS, K_MINUS, K_MUL, K_DIV, K_EQ, K_CLR, K_BS, K_NONE
} key_id_t;

typedef struct {
    key_id_t id;
    const char *label;
    uint32_t accent;    /* 0 = digit/neutral */
} key_t;

// Accent roles: red = clear/bs, yellow = operators, blue = equals.
// C ⌫ / x
// 7 8 9 -
// 4 5 6 +
// 1 2 3 =
// 0 . =   (K_NONE placeholder under wide "0"; skipped when navigating)
static const key_t k_keys[ROWS][COLS] = {
    { { K_CLR,   "C",  TOY_RED }, { K_BS,    LV_SYMBOL_BACKSPACE, TOY_RED },
      { K_DIV,   "/",  TOY_YELLOW }, { K_MUL,   "x",  TOY_YELLOW } },
    { { K_7,     "7",  0 },          { K_8,     "8",  0 },
      { K_9,     "9",  0 },          { K_MINUS, "-",  TOY_YELLOW } },
    { { K_4,     "4",  0 },          { K_5,     "5",  0 },
      { K_6,     "6",  0 },          { K_PLUS,  "+",  TOY_YELLOW } },
    { { K_1,     "1",  0 },          { K_2,     "2",  0 },
      { K_3,     "3",  0 },          { K_EQ,    "=",  TOY_BLUE } },
    { { K_0,     "0",  0 },          { K_NONE,  "",   0 },
      { K_DOT,   ".",  0 },          { K_EQ,    "=",  TOY_BLUE } },
};

static lv_obj_t *s_root, *s_disp, *s_expr, *s_hint, *s_disp_panel;
static lv_obj_t *s_btn[ROWS][COLS];
static lv_obj_t *s_btn_lbl[ROWS][COLS];

static char s_entry[ENTRY_MAX];
static double s_acc;
static char s_op;               /* 0, '+', '-', '*', '/' */
static bool s_fresh;            /* next digit replaces entry */
static bool s_error;
static int s_row, s_col;

// ---- formatting -----------------------------------------------------------

static void set_entry_str(const char *s)
{
    strlcpy(s_entry, s, sizeof(s_entry));
}

static void format_number(double v, char *out, size_t n)
{
    if (!isfinite(v)) {
        strlcpy(out, "Error", n);
        return;
    }
    // Prefer a short fixed/scientific rendering that fits the display.
    char tmp[48];
    if (fabs(v) >= 1e10 || (fabs(v) > 0 && fabs(v) < 1e-6)) {
        snprintf(tmp, sizeof(tmp), "%.6g", v);
    } else {
        snprintf(tmp, sizeof(tmp), "%.10g", v);
    }
    // Trim trailing zeros after a decimal point.
    char *dot = strchr(tmp, '.');
    if (dot && !strchr(tmp, 'e') && !strchr(tmp, 'E')) {
        char *end = tmp + strlen(tmp) - 1;
        while (end > dot && *end == '0') {
            *end-- = 0;
        }
        if (*end == '.') {
            *end = 0;
        }
    }
    if (strlen(tmp) >= n) {
        snprintf(tmp, sizeof(tmp), "%.4g", v);
    }
    strlcpy(out, tmp, n);
}

static void refresh_disp(void)
{
    if (!s_disp) {
        return;
    }
    if (s_error) {
        lv_label_set_text(s_disp, "Error");
        lv_obj_set_style_text_color(s_disp, lv_color_hex(TOY_RED), 0);
    } else {
        lv_label_set_text(s_disp, s_entry[0] ? s_entry : "0");
        lv_obj_set_style_text_color(s_disp, lv_color_hex(TOY_INK), 0);
    }

    if (s_expr) {
        if (s_op && !s_error) {
            char acc[DISP_MAX];
            format_number(s_acc, acc, sizeof(acc));
            char opch = s_op == '*' ? 'x' : s_op;
            lv_label_set_text_fmt(s_expr, "%s %c", acc, opch);
            lv_obj_set_style_text_color(s_expr, lv_color_hex(TOY_BLUE), 0);
        } else {
            lv_label_set_text(s_expr, "");
        }
    }
}

// Focus = yellow fill + red rim (always visible, even on yellow ops). Digits
// sit on cream with a blue rim; ops / clear / = are solid primaries.
static void paint_keys(void)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (k_keys[r][c].id == K_NONE || !s_btn[r][c]) {
                continue;
            }
            bool hi = (r == s_row && c == s_col);
            uint32_t accent = k_keys[r][c].accent;
            uint32_t bg, fg, border;
            int border_w;
            if (hi) {
                bg = TOY_YELLOW;
                fg = TOY_INK;
                border = TOY_RED;
                border_w = 2;
            } else if (accent == TOY_YELLOW) {
                bg = TOY_YELLOW;
                fg = TOY_INK;
                border = TOY_YELLOW;
                border_w = 0;
            } else if (accent == TOY_RED) {
                bg = TOY_RED;
                fg = TOY_CREAM;
                border = TOY_RED;
                border_w = 0;
            } else if (accent == TOY_BLUE) {
                bg = TOY_BLUE;
                fg = TOY_CREAM;
                border = TOY_BLUE;
                border_w = 0;
            } else {
                bg = TOY_KEY;
                fg = TOY_INK;
                border = TOY_BLUE;
                border_w = 1;
            }
            lv_obj_set_style_bg_color(s_btn[r][c], lv_color_hex(bg), 0);
            lv_obj_set_style_border_color(s_btn[r][c], lv_color_hex(border), 0);
            lv_obj_set_style_border_width(s_btn[r][c], border_w, 0);
            if (s_btn_lbl[r][c]) {
                lv_obj_set_style_text_color(s_btn_lbl[r][c], lv_color_hex(fg), 0);
            }
        }
    }
}

static bool cell_ok(int r, int c)
{
    return r >= 0 && r < ROWS && c >= 0 && c < COLS && k_keys[r][c].id != K_NONE;
}

static void move_sel(int dr, int dc)
{
    int r = s_row, c = s_col;
    for (int i = 0; i < ROWS * COLS; i++) {
        r += dr;
        c += dc;
        if (r < 0) {
            r = ROWS - 1;
        } else if (r >= ROWS) {
            r = 0;
        }
        if (c < 0) {
            c = COLS - 1;
        } else if (c >= COLS) {
            c = 0;
        }
        if (cell_ok(r, c)) {
            s_row = r;
            s_col = c;
            paint_keys();
            return;
        }
    }
}

// ---- calculator logic -----------------------------------------------------

static void clear_all(void)
{
    s_acc = 0;
    s_op = 0;
    s_fresh = true;
    s_error = false;
    set_entry_str("0");
}

static void backspace(void)
{
    if (s_error) {
        clear_all();
        return;
    }
    // After = or an operator, CANCEL/⌫ clears the shown value back to 0.
    if (s_fresh) {
        if (s_op) {
            s_op = 0;
        }
        set_entry_str("0");
        s_fresh = true;
        s_acc = 0;
        return;
    }
    size_t n = strlen(s_entry);
    if (n <= 1 || (n == 2 && s_entry[0] == '-')) {
        set_entry_str("0");
        s_fresh = true;
        return;
    }
    s_entry[n - 1] = 0;
}

static double entry_value(void)
{
    return atof(s_entry);
}

static void apply_op(void)
{
    double rhs = entry_value();
    if (!s_op) {
        s_acc = rhs;
        return;
    }
    switch (s_op) {
    case '+':
        s_acc += rhs;
        break;
    case '-':
        s_acc -= rhs;
        break;
    case '*':
        s_acc *= rhs;
        break;
    case '/':
        if (rhs == 0.0) {
            s_error = true;
            return;
        }
        s_acc /= rhs;
        break;
    }
    if (!isfinite(s_acc)) {
        s_error = true;
        return;
    }
    char buf[DISP_MAX];
    format_number(s_acc, buf, sizeof(buf));
    set_entry_str(buf);
}

static void press_digit(char d)
{
    if (s_error) {
        clear_all();
    }
    if (s_fresh) {
        s_entry[0] = d;
        s_entry[1] = 0;
        s_fresh = false;
        return;
    }
    if (strcmp(s_entry, "0") == 0 && d != '.') {
        s_entry[0] = d;
        s_entry[1] = 0;
        return;
    }
    if (strlen(s_entry) >= ENTRY_MAX - 1) {
        return;
    }
    size_t n = strlen(s_entry);
    s_entry[n] = d;
    s_entry[n + 1] = 0;
}

static void press_dot(void)
{
    if (s_error) {
        clear_all();
    }
    if (s_fresh) {
        set_entry_str("0.");
        s_fresh = false;
        return;
    }
    if (strchr(s_entry, '.')) {
        return;
    }
    if (strlen(s_entry) >= ENTRY_MAX - 1) {
        return;
    }
    size_t n = strlen(s_entry);
    s_entry[n] = '.';
    s_entry[n + 1] = 0;
}

static void press_op(char op)
{
    if (s_error) {
        clear_all();
        return;
    }
    if (!s_fresh || !s_op) {
        apply_op();
        if (s_error) {
            refresh_disp();
            return;
        }
    }
    s_op = op;
    s_fresh = true;
}

static void press_eq(void)
{
    if (s_error) {
        clear_all();
        refresh_disp();
        return;
    }
    if (!s_op) {
        return;
    }
    apply_op();
    s_op = 0;
    s_fresh = true;
}

static void activate(key_id_t id)
{
    switch (id) {
    case K_0: case K_1: case K_2: case K_3: case K_4:
    case K_5: case K_6: case K_7: case K_8: case K_9:
        press_digit((char)('0' + (id - K_0)));
        break;
    case K_DOT:
        press_dot();
        break;
    case K_PLUS:
        press_op('+');
        break;
    case K_MINUS:
        press_op('-');
        break;
    case K_MUL:
        press_op('*');
        break;
    case K_DIV:
        press_op('/');
        break;
    case K_EQ:
        press_eq();
        break;
    case K_CLR:
        clear_all();
        break;
    case K_BS:
        backspace();
        break;
    default:
        break;
    }
    refresh_disp();
}

// ---- UI -------------------------------------------------------------------

static void build_keys(lv_obj_t *root)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            s_btn[r][c] = NULL;
            s_btn_lbl[r][c] = NULL;
            if (k_keys[r][c].id == K_NONE) {
                continue;
            }
            int x = GRID_X + c * (KEY_W + KEY_GAP);
            int y = GRID_Y + r * (KEY_H + KEY_GAP);
            // Widen "0" across the empty placeholder cell beside it.
            int w = KEY_W;
            if (k_keys[r][c].id == K_0 && c + 1 < COLS && k_keys[r][c + 1].id == K_NONE) {
                w = KEY_W * 2 + KEY_GAP;
            }
            lv_obj_t *b = lv_obj_create(root);
            lv_obj_remove_style_all(b);
            lv_obj_set_pos(b, x, y);
            lv_obj_set_size(b, w, KEY_H);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_radius(b, KEY_RADIUS, 0);
            lv_obj_set_scrollable(b, false);

            lv_obj_t *lbl = ui_label(b, &sol_font_20, COL_TEXT, k_keys[r][c].label);
            lv_obj_set_style_text_color(lbl, lv_color_hex(TOY_INK), 0);
            lv_obj_center(lbl);

            s_btn[r][c] = b;
            s_btn_lbl[r][c] = lbl;
        }
    }
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    clear_all();
    s_row = 1;
    s_col = 0;  /* start on "7" */

    // Deep-blue faceplate behind the pad (true colour, not theme-remapped).
    lv_obj_set_style_bg_color(root, lv_color_hex(TOY_FACE), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    // Readout: cream field in a thick red rounded bezel — high-contrast ink.
    s_disp_panel = ui_panel(root, DISP_X, DISP_Y, OS_ROOT_W - 2 * DISP_X, DISP_H - 4);
    lv_obj_set_style_bg_color(s_disp_panel, lv_color_hex(TOY_CREAM), 0);
    lv_obj_set_style_border_color(s_disp_panel, lv_color_hex(TOY_RED), 0);
    lv_obj_set_style_border_width(s_disp_panel, 3, 0);
    lv_obj_set_style_radius(s_disp_panel, DISP_RADIUS, 0);
    lv_obj_set_style_pad_all(s_disp_panel, 4, 0);

    s_expr = ui_label(s_disp_panel, &sol_mono_12, COL_DIM, "");
    lv_obj_set_style_text_color(s_expr, lv_color_hex(TOY_BLUE), 0);
    lv_obj_align(s_expr, LV_ALIGN_TOP_RIGHT, -2, 0);

    s_disp = ui_label(s_disp_panel, &sol_font_32, COL_TEXT, "0");
    lv_obj_set_style_text_color(s_disp, lv_color_hex(TOY_INK), 0);
    lv_obj_set_style_text_align(s_disp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_disp, OS_ROOT_W - 2 * DISP_X - 20);
    lv_obj_align(s_disp, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_label_set_long_mode(s_disp, LV_LABEL_LONG_CLIP);

    // Yellow accent strip under the bezel (replaces Solana brand gradient).
    lv_obj_t *strip = lv_obj_create(root);
    lv_obj_remove_style_all(strip);
    lv_obj_set_pos(strip, DISP_X + 4, DISP_Y + DISP_H - 5);
    lv_obj_set_size(strip, OS_ROOT_W - 2 * DISP_X - 8, 3);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(TOY_YELLOW), 0);
    lv_obj_set_style_radius(strip, 2, 0);
    lv_obj_set_scrollable(strip, false);

    build_keys(root);
    paint_keys();
    refresh_disp();

    s_hint = ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
                     " move  SELECT key  CANCEL bs/exit");
    if (s_hint) {
        lv_obj_set_style_text_color(s_hint, lv_color_hex(TOY_HINT), 0);
    }
}

static void leave(void)
{
    s_root = s_disp = s_expr = s_hint = s_disp_panel = NULL;
    memset(s_btn, 0, sizeof(s_btn));
    memset(s_btn_lbl, 0, sizeof(s_btn_lbl));
}

static bool on_key(os_key_t k, bool repeat)
{
    if (k == OS_KEY_UP) {
        move_sel(-1, 0);
        return true;
    }
    if (k == OS_KEY_DOWN) {
        move_sel(1, 0);
        return true;
    }
    if (k == OS_KEY_LEFT) {
        move_sel(0, -1);
        return true;
    }
    if (k == OS_KEY_RIGHT) {
        move_sel(0, 1);
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        if (cell_ok(s_row, s_col)) {
            activate(k_keys[s_row][s_col].id);
        }
        return true;
    }
    if (k == OS_KEY_BACK && !repeat) {
        // Backspace when there is something to erase; otherwise leave the app.
        bool empty = s_fresh && (!s_entry[0] || strcmp(s_entry, "0") == 0) && !s_op && !s_error;
        if (empty) {
            return false;
        }
        backspace();
        // If backspace left a pristine zero with no pending op, treat a second
        // CANCEL as exit — but first CANCEL always clears one step.
        if (s_error) {
            clear_all();
        }
        refresh_disp();
        return true;
    }
    return k != OS_KEY_BACK;
}

const os_app_t app_calc = {
    .id = "calc",
    .name = "Calculator",
    .symbol = LV_SYMBOL_EDIT,
    .accent = TOY_RED,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
};
