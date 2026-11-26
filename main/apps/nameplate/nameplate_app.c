// Nameplate: show a badge wearer's name and title on the ILI9341 with a
// Contact QR that opens the live personal contact page. Name and title are
// editable on device and stored in NVS so they survive reboots.
//
// Visual theme: shared pocket-battle palette (sky stage, cream cards, navy
// chrome). Original colours only — no copyrighted creature art or names.

#include <stdio.h>
#include <string.h>
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

enum { FIELD_NAME, FIELD_TITLE, FIELD_COUNT };

static lv_obj_t *s_root, *s_view, *s_hint;
static lv_obj_t *s_name_lbl, *s_title_lbl, *s_field_hint;
static ui_kb_t *s_kb;
static char s_name[NAME_MAX];
static char s_title[TITLE_MAX];
static int s_field;

static lv_obj_t *label_at(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          const char *text, int x, int y, int w)
{
    lv_obj_t *l = battle_label(parent, font, color, text);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    return l;
}

static void load_settings(void)
{
    strlcpy(s_name, DEFAULT_NAME, sizeof(s_name));
    strlcpy(s_title, DEFAULT_TITLE, sizeof(s_title));

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
        battle_set_hint(s_hint, "UP/DOWN field   SELECT edit name   CANCEL exit");
    } else {
        lv_label_set_text(s_field_hint, "  Name   > Title");
        battle_set_hint(s_hint, "UP/DOWN field   SELECT edit title   CANCEL exit");
    }
}

static void refresh_text(void)
{
    if (s_name_lbl) {
        lv_label_set_text(s_name_lbl, s_name[0] ? s_name : "(name)");
        lv_obj_set_style_text_color(s_name_lbl,
                                    lv_color_hex(s_name[0] ? T_INK : T_DIM), 0);
    }
    if (s_title_lbl) {
        lv_label_set_text(s_title_lbl, s_title[0] ? s_title : "(title)");
        lv_obj_set_style_text_color(s_title_lbl,
                                    lv_color_hex(s_title[0] ? T_BLUE : T_DIM), 0);
    }
    update_field_hint();
}

static void build(void)
{
    if (s_view) {
        lv_obj_delete(s_view);
    }
    s_name_lbl = s_title_lbl = s_field_hint = NULL;
    s_view = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, OS_ROOT_W, CONTENT_H);
    lv_obj_set_scrollable(s_view, false);

    lv_obj_t *qr_card = battle_card(s_view, 8, 10, QR_BOX, QR_BOX, T_TEAL);
    lv_obj_t *qr = lv_qrcode_create(qr_card);
    lv_qrcode_set_size(qr, QR_SIZE);
    lv_qrcode_set_dark_color(qr, lv_color_hex(T_INK));
    lv_qrcode_set_light_color(qr, lv_color_hex(T_FACE));
    if (lv_qrcode_update(qr, CONTACT_PAGE_URL, strlen(CONTACT_PAGE_URL)) != LV_RESULT_OK) {
        lv_obj_delete(qr);
        lv_obj_t *err = label_at(qr_card, &sol_font_14, T_RED,
                                 "Link too long\nfor a QR code", 0, 0, 0);
        lv_obj_center(err);
    } else {
        lv_obj_center(qr);
    }

    lv_obj_t *cap = label_at(s_view, &sol_mono_12, T_NAVY, "Scan for Contact",
                             4, 10 + QR_BOX + 4, QR_BOX + 8);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *info = battle_card(s_view, COL_X - 4, 8, COL_W + 8, CONTENT_H - 36, T_YELLOW);
    label_at(info, &sol_mono_12, T_DIM, "NAMEPLATE", 8, BATTLE_STRIPE_H + 6, COL_W - 8);
    s_name_lbl = label_at(info, &sol_font_32, T_INK, "", 8, BATTLE_STRIPE_H + 26, COL_W - 8);
    s_title_lbl = label_at(info, &sol_font_20, T_BLUE, "", 8, BATTLE_STRIPE_H + 74, COL_W - 8);

    label_at(info, &sol_mono_12, T_DIM, "EDIT (NVS)", 8, BATTLE_STRIPE_H + 112, COL_W - 8);
    s_field_hint = label_at(info, &sol_mono_12, T_INK, "", 8, BATTLE_STRIPE_H + 130, COL_W - 8);
    label_at(info, &sol_mono_12, T_DIM, "Saved on this badge", 8, BATTLE_STRIPE_H + 150, COL_W - 8);

    refresh_text();
}

static void open_kb(void)
{
    const char *prompt = s_field == FIELD_NAME ? "Nameplate name" : "Nameplate title";
    size_t max = s_field == FIELD_NAME ? NAME_MAX - 1 : TITLE_MAX - 1;
    s_kb = ui_kb_create(s_root, prompt, max);
}

static void on_enter(lv_obj_t *root)
{
    s_root = root;
    s_view = NULL;
    s_kb = NULL;
    s_field = FIELD_NAME;
    load_settings();
    battle_paint_stage(root);
    s_hint = battle_hint(root, "");
    build();
}

static void on_exit(void)
{
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    s_root = s_view = s_hint = NULL;
    s_name_lbl = s_title_lbl = s_field_hint = NULL;
}

static void on_tick(void)
{
    if (s_kb) {
        return;
    }
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
    .accent = T_TEAL,
    .on_enter = on_enter,
    .on_exit = on_exit,
    .on_key = on_key,
    .on_tick = on_tick,
};
