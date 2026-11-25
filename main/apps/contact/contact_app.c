// Contact: Dave Cheng's personal contact card on the badge.
//
// Shows a snapshot of https://masterasnackin.github.io/personal-contact-page/
// (from MasteraSnackin/personal-contact-page) plus a QR code of that live
// page URL so a phone can open it. Content is embedded so the app works
// offline; the status line notes Wi-Fi when joined. Always draw the QR.

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/os_wifi.h"
#include "core/ui.h"

#define CONTENT_H   (OS_ROOT_H - UI_HINT_H)
#define QR_BOX      150
#define QR_SIZE     134
#define COL_X       (10 + QR_BOX + 10)
#define COL_W       (OS_ROOT_W - COL_X - 8)

// Live site documented on the GitHub repo (GitHub Pages). Prefer this over
// the repository URL so a scan opens the contact page itself.
#define CONTACT_PAGE_URL "https://masterasnackin.github.io/personal-contact-page/"

typedef struct {
    const char *label;
    const char *detail;
    const char *url;            // NULL for WeChat (ID only)
} contact_link_t;

// Snapshot of index.html from MasteraSnackin/personal-contact-page (main).
static const char *const s_name = "Dave Cheng";
static const char *const s_title = "Get in touch";
static const char *const s_bio = "Choose a way to connect.";

static const contact_link_t s_links[] = {
    {"WhatsApp",  "+447727 821 753",           "https://wa.me/447727821753"},
    {"LinkedIn",  "davecheng82",               "https://www.linkedin.com/in/davecheng82"},
    {"GitHub",    "masterasnackin",            "https://github.com/masterasnackin"},
    {"Telegram",  "@Masterasnackin",           "https://t.me/Masterasnackin"},
    {"X",         "@Masterasnackin",           "https://x.com/Masterasnackin"},
    {"WeChat",    "masterasnackin",            NULL},
    {"Email",     "Mythicmindlabs@gmail.com",  "mailto:Mythicmindlabs@gmail.com"},
};
#define LINK_COUNT  ((int)(sizeof(s_links) / sizeof(s_links[0])))

static lv_obj_t *s_root, *s_view, *s_hint, *s_qr;
static lv_obj_t *s_link_label, *s_link_detail, *s_link_idx, *s_status;
static int s_sel;
static bool s_light;

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

static void update_link(void)
{
    const contact_link_t *c = &s_links[s_sel];
    char idx[16];
    lv_label_set_text(s_link_label, c->label);
    lv_label_set_text(s_link_detail, c->detail);
    snprintf(idx, sizeof(idx), "%d / %d", s_sel + 1, LINK_COUNT);
    lv_label_set_text(s_link_idx, idx);
}

static void update_status(void)
{
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    if (w.state == OS_WIFI_CONNECTED) {
        lv_label_set_text(s_status, "Wi-Fi on  ·  snapshot");
        lv_obj_set_style_text_color(s_status, ui_col(COL_GREEN), 0);
    } else if (os_wifi_enabled()) {
        lv_label_set_text(s_status, "Joining Wi-Fi...");
        lv_obj_set_style_text_color(s_status, ui_col(COL_ORANGE), 0);
    } else {
        lv_label_set_text(s_status, "Offline  ·  snapshot");
        lv_obj_set_style_text_color(s_status, ui_col(COL_DIM), 0);
    }
}

static void build(void)
{
    if (s_view) {
        lv_obj_delete(s_view);
    }
    s_qr = s_link_label = s_link_detail = s_link_idx = s_status = NULL;
    s_view = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, OS_ROOT_W, CONTENT_H);
    lv_obj_set_scrollable(s_view, false);

    bool light = ui_light();
    lv_color_t fg = light ? lv_color_black() : lv_color_white();
    lv_color_t bg = light ? lv_color_white() : lv_color_black();

    lv_obj_t *box = lv_obj_create(s_view);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, QR_BOX, QR_BOX);
    lv_obj_set_pos(box, 10, 8);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, bg, 0);
    lv_obj_set_style_radius(box, 8, 0);
    if (!light) {
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, ui_col(COL_BORDER), 0);
    }
    lv_obj_set_scrollable(box, false);

    s_qr = lv_qrcode_create(box);
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, fg);
    lv_qrcode_set_light_color(s_qr, bg);
    if (lv_qrcode_update(s_qr, CONTACT_PAGE_URL, strlen(CONTACT_PAGE_URL)) != LV_RESULT_OK) {
        lv_obj_delete(s_qr);
        s_qr = NULL;
        lv_obj_t *err = label_at(box, &sol_font_14, light ? COL_BG : COL_TEXT,
                                 "Link too long\nfor a QR code", 0, 0, 0);
        lv_obj_center(err);
    } else {
        lv_obj_center(s_qr);
    }

    lv_obj_t *cap = label_at(s_view, &sol_mono_12, COL_DIM, "Scan for the page", 6, 8 + QR_BOX + 4, QR_BOX + 8);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    label_at(s_view, &sol_mono_12, COL_DIM, "CONTACT", COL_X, 8, COL_W);
    label_at(s_view, &sol_font_20, COL_TEXT, s_name, COL_X, 24, COL_W);
    label_at(s_view, &sol_font_14, COL_DIM, s_title, COL_X, 50, COL_W);
    label_at(s_view, &sol_mono_12, COL_DIM, s_bio, COL_X, 70, COL_W);

    ui_gradient_bar(s_view, COL_X, 92, COL_W, 3);

    label_at(s_view, &sol_mono_12, COL_DIM, "CHANNEL", COL_X, 102, COL_W);
    s_link_label = label_at(s_view, &sol_font_20, COL_AQUA, "", COL_X, 116, COL_W);
    s_link_detail = label_at(s_view, &sol_font_14, COL_TEXT, "", COL_X, 142, COL_W);
    s_link_idx = label_at(s_view, &sol_mono_12, COL_DIM, "", COL_X, 166, COL_W);
    s_status = label_at(s_view, &sol_mono_12, COL_DIM, "", COL_X, CONTENT_H - 18, COL_W);

    update_link();
    update_status();
    s_light = light;
}

static void on_enter(lv_obj_t *root)
{
    s_root = root;
    s_view = NULL;
    s_sel = 0;
    s_hint = ui_hint(root, "UP/DOWN channel   SELECT detail   CANCEL exit");
    build();
}

static void on_exit(void)
{
    s_root = s_view = s_hint = s_qr = NULL;
    s_link_label = s_link_detail = s_link_idx = s_status = NULL;
}

static void on_tick(void)
{
    if (ui_light() != s_light) {
        build();
        return;
    }
    update_status();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (k == OS_KEY_UP) {
        s_sel = (s_sel + LINK_COUNT - 1) % LINK_COUNT;
        update_link();
        return true;
    }
    if (k == OS_KEY_DOWN) {
        s_sel = (s_sel + 1) % LINK_COUNT;
        update_link();
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        const contact_link_t *c = &s_links[s_sel];
        if (c->url) {
            os_toast("%s: %s", c->label, c->detail);
        } else {
            os_toast("WeChat ID: %s", c->detail);
        }
        return true;
    }
    return false;
}

const os_app_t app_contact = {
    .id = "contact",
    .name = "Contact",
    .symbol = LV_SYMBOL_HOME,
    .accent = COL_AQUA,
    .on_enter = on_enter,
    .on_exit = on_exit,
    .on_key = on_key,
    .on_tick = on_tick,
};
