// Tangem: honest info panel for connecting a Tangem NFC card to BP26.
//
// BP26 has no NFC reader (see components/bsp/include/board.h). Tangem cards
// are NFC-only (no BLE/USB). This app does not pretend otherwise: it explains
// the limit and shows a QR to the official Tangem site so a phone can open
// the companion app. No card tap, no signing on the badge.

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/ui.h"

#define CONTENT_H   (OS_ROOT_H - UI_HINT_H)
#define QR_BOX      150
#define QR_SIZE     134
#define COL_X       (10 + QR_BOX + 10)
#define COL_W       (OS_ROOT_W - COL_X - 8)

// Official site (Tangem: start downloads from tangem.com, not random QR ads).
#define TANGEM_URL  "https://tangem.com"

typedef struct {
    const char *label;
    const char *detail;
    const char *toast;
} topic_t;

static const topic_t s_topics[] = {
    {
        "MCP first",
        "Mac Cursor: server tangem → mcp.tangem.org via mcp-remote. Badge is UI.",
        "Use Tangem MCP on Mac — badge does not tap the card",
    },
    {
        "No NFC here",
        "BP26 cannot tap a Tangem card. Keys stay on the card; phones talk NFC.",
        "Badge has no NFC reader — use phone or mcp.tangem.org",
    },
    {
        "Phone + MCP",
        "Official portal mcp.tangem.org. OAuth + card tap still required to sign.",
        "MCP prepares; NFC tap authorizes",
    },
    {
        "WalletConnect",
        "Fallback: dApps via WalletConnect QR. Card tap signs on phone.",
        "Scan WC QR in Tangem app, then tap card",
    },
    {
        "Not BLE",
        "Tangem has no Bluetooth. Badge BLE cannot reach the card.",
        "Card is NFC-only — no BLE pairing",
    },
};
#define TOPIC_COUNT ((int)(sizeof(s_topics) / sizeof(s_topics[0])))

static lv_obj_t *s_root, *s_view, *s_hint, *s_qr;
static lv_obj_t *s_topic, *s_detail, *s_idx, *s_status;
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

static void update_topic(void)
{
    const topic_t *t = &s_topics[s_sel];
    char idx[16];
    lv_label_set_text(s_topic, t->label);
    lv_label_set_text(s_detail, t->detail);
    snprintf(idx, sizeof(idx), "%d / %d", s_sel + 1, TOPIC_COUNT);
    lv_label_set_text(s_idx, idx);
}

static void build(void)
{
    if (s_view) {
        lv_obj_delete(s_view);
    }
    s_qr = s_topic = s_detail = s_idx = s_status = NULL;
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
    if (lv_qrcode_update(s_qr, TANGEM_URL, strlen(TANGEM_URL)) != LV_RESULT_OK) {
        lv_obj_delete(s_qr);
        s_qr = NULL;
        lv_obj_t *err = label_at(box, &sol_font_14, light ? COL_BG : COL_TEXT,
                                 "Link too long\nfor a QR code", 0, 0, 0);
        lv_obj_center(err);
    } else {
        lv_obj_center(s_qr);
    }

    lv_obj_t *cap = label_at(s_view, &sol_mono_12, COL_DIM, "Scan: tangem.com", 6, 8 + QR_BOX + 4, QR_BOX + 8);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    label_at(s_view, &sol_mono_12, COL_DIM, "TANGEM", COL_X, 8, COL_W);
    label_at(s_view, &sol_font_20, COL_TEXT, "Hardware card", COL_X, 24, COL_W);
    label_at(s_view, &sol_font_14, COL_ORANGE, "NFC only · no badge tap", COL_X, 50, COL_W);

    ui_gradient_bar(s_view, COL_X, 72, COL_W, 3);

    label_at(s_view, &sol_mono_12, COL_DIM, "TOPIC", COL_X, 82, COL_W);
    s_topic = label_at(s_view, &sol_font_20, COL_AQUA, "", COL_X, 96, COL_W);
    s_detail = label_at(s_view, &sol_mono_12, COL_TEXT, "", COL_X, 122, COL_W);
    s_idx = label_at(s_view, &sol_mono_12, COL_DIM, "", COL_X, 178, COL_W);
    s_status = label_at(s_view, &sol_mono_12, COL_DIM, "Keys never leave the card", COL_X, CONTENT_H - 18, COL_W);

    update_topic();
    s_light = light;
}

static void on_enter(lv_obj_t *root)
{
    s_root = root;
    s_view = NULL;
    s_sel = 0;
    s_hint = ui_hint(root, "UP/DOWN topic   SELECT tip   CANCEL exit");
    build();
}

static void on_exit(void)
{
    s_root = s_view = s_hint = s_qr = NULL;
    s_topic = s_detail = s_idx = s_status = NULL;
}

static void on_tick(void)
{
    if (ui_light() != s_light) {
        build();
    }
}

static bool on_key(os_key_t k, bool repeat)
{
    if (k == OS_KEY_UP) {
        s_sel = (s_sel + TOPIC_COUNT - 1) % TOPIC_COUNT;
        update_topic();
        return true;
    }
    if (k == OS_KEY_DOWN) {
        s_sel = (s_sel + 1) % TOPIC_COUNT;
        update_topic();
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        os_toast("%s", s_topics[s_sel].toast);
        return true;
    }
    return false;
}

const os_app_t app_tangem = {
    .id = "tangem",
    .name = "Tangem",
    .symbol = LV_SYMBOL_SETTINGS,
    .accent = COL_AQUA,
    .on_enter = on_enter,
    .on_exit = on_exit,
    .on_key = on_key,
    .on_tick = on_tick,
};
