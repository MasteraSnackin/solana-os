// Simple Wi-Fi text browser for BP26: fetch HTTP(S), strip HTML to plain text,
// scroll on the 320×240 panel. URL entry uses the shared d-pad keyboard
// (ui_kb). Needs Wi-Fi; reuses ui_offline when disconnected. Fetch runs on a
// worker via os_http_request (not Chromium / not a full browser).

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bsp.h"
#include "core/os.h"
#include "core/os_cmd.h"
#include "core/os_http.h"
#include "core/os_mem.h"
#include "core/os_wifi.h"
#include "core/ui.h"

static const char *TAG = "browser";

#define NVS_NS          "browser"
#define URL_FILE        BSP_SD_MOUNT "/browser.url"
#define URL_MAX         192
#define HTML_MAX        (96 * 1024)
#define TEXT_MAX        (64 * 1024)
#define SCROLL_STEP     40
#define CONTENT_H       (OS_ROOT_H - UI_HINT_H)
#define WORKER_STACK    8192

typedef enum {
    V_HOME = 0,
    V_READ,
} view_t;

typedef enum {
    FETCH_IDLE = 0,
    FETCH_BUSY,
    FETCH_OK,
    FETCH_FAIL,
} fetch_t;

static lv_obj_t *s_root, *s_hint, *s_offline, *s_home_title, *s_home_url, *s_home_st;
static lv_obj_t *s_scroll, *s_title, *s_body;
static ui_kb_t *s_kb;

static view_t s_view;
static char s_url[URL_MAX];
static char *s_text;
static size_t s_text_len;

static volatile fetch_t s_fetch;
static char s_err[96];
static int s_http_status;
static char *s_pending_text;            // worker → UI; taken on tick
static size_t s_pending_len;
static char s_pending_url[URL_MAX];

// ---- small helpers --------------------------------------------------------

static void trim_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *a = s;
    while (*a && isspace((unsigned char)*a)) {
        a++;
    }
    if (a != s) {
        memmove(s, a, strlen(a) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = 0;
    }
}

static bool read_file_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    if (!fgets(out, (int)cap, f)) {
        out[0] = 0;
        fclose(f);
        return false;
    }
    fclose(f);
    trim_inplace(out);
    return out[0] != 0;
}

static bool nvs_get_url(char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, "url", out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = 0;
        return false;
    }
    trim_inplace(out);
    return out[0] != 0;
}

static void nvs_set_url(const char *url)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "url", url ? url : "");
    nvs_commit(h);
    nvs_close(h);
}

static void load_url(void)
{
    s_url[0] = 0;
    if (!nvs_get_url(s_url, sizeof(s_url))) {
        read_file_line(URL_FILE, s_url, sizeof(s_url));
    }
}

static void normalize_url(char *url, size_t cap)
{
    trim_inplace(url);
    if (!url[0]) {
        return;
    }
    if (!strstr(url, "://")) {
        char tmp[URL_MAX];
        snprintf(tmp, sizeof(tmp), "https://%s", url);
        strlcpy(url, tmp, cap);
    }
}

static bool wifi_ready(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED;
}

// ---- HTML → plain text (same idea as ebook) -------------------------------

static void append_ch(char **dst, size_t *len, size_t *cap, char c)
{
    if (*len + 1 >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 4096;
        if (ncap > TEXT_MAX + 1) {
            ncap = TEXT_MAX + 1;
        }
        if (*len + 1 >= ncap) {
            return;
        }
        char *n = heap_caps_realloc(*dst, ncap, os_mem_bulk_caps());
        if (!n) {
            return;
        }
        *dst = n;
        *cap = ncap;
    }
    (*dst)[(*len)++] = c;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static const char *decode_entity(const char *p, char *out)
{
    if (!strncmp(p, "&amp;", 5)) {
        *out = '&';
        return p + 5;
    }
    if (!strncmp(p, "&lt;", 4)) {
        *out = '<';
        return p + 4;
    }
    if (!strncmp(p, "&gt;", 4)) {
        *out = '>';
        return p + 4;
    }
    if (!strncmp(p, "&quot;", 6)) {
        *out = '"';
        return p + 6;
    }
    if (!strncmp(p, "&apos;", 6)) {
        *out = '\'';
        return p + 6;
    }
    if (!strncmp(p, "&nbsp;", 6)) {
        *out = ' ';
        return p + 6;
    }
    if (p[1] == '#' && (p[2] == 'x' || p[2] == 'X')) {
        int v = 0, i = 3;
        for (; i < 8 && hex_val(p[i]) >= 0; i++) {
            v = (v << 4) | hex_val(p[i]);
        }
        if (p[i] == ';' && v > 0 && v < 128) {
            *out = (char)v;
            return p + i + 1;
        }
    } else if (p[1] == '#') {
        int v = 0, i = 2;
        for (; i < 8 && p[i] >= '0' && p[i] <= '9'; i++) {
            v = v * 10 + (p[i] - '0');
        }
        if (p[i] == ';' && v > 0 && v < 128) {
            *out = (char)v;
            return p + i + 1;
        }
    }
    *out = '&';
    return p + 1;
}

static bool tag_is(const char *tag, const char *name)
{
    size_t n = strlen(name);
    return !strncasecmp(tag, name, n) &&
           (tag[n] == 0 || tag[n] == ' ' || tag[n] == '/' || tag[n] == '\t' ||
            tag[n] == '\n' || tag[n] == '\r');
}

static void html_to_text(const char *html, char **out, size_t *out_len)
{
    char *dst = NULL;
    size_t len = 0, cap = 0;
    bool in_script = false, in_style = false;
    int nl_run = 0;

    for (const char *p = html; *p && len < TEXT_MAX;) {
        if (*p == '<') {
            const char *end = strchr(p, '>');
            if (!end) {
                break;
            }
            const char *tag = p + 1;
            while (*tag == ' ' || *tag == '\t') {
                tag++;
            }
            bool close = (*tag == '/');
            if (close) {
                tag++;
            }
            if (tag_is(tag, "script")) {
                in_script = !close;
            } else if (tag_is(tag, "style")) {
                in_style = !close;
            } else if (!in_script && !in_style) {
                if (tag_is(tag, "br") || tag_is(tag, "p") || tag_is(tag, "div") ||
                    tag_is(tag, "tr") || tag_is(tag, "li") || tag_is(tag, "h1") ||
                    tag_is(tag, "h2") || tag_is(tag, "h3") || tag_is(tag, "h4") ||
                    tag_is(tag, "blockquote") || tag_is(tag, "section") ||
                    tag_is(tag, "hr") || tag_is(tag, "title")) {
                    if (nl_run < 2) {
                        append_ch(&dst, &len, &cap, '\n');
                        nl_run++;
                    }
                } else if (tag_is(tag, "td") || tag_is(tag, "th")) {
                    append_ch(&dst, &len, &cap, ' ');
                    nl_run = 0;
                }
            }
            p = end + 1;
            continue;
        }
        if (in_script || in_style) {
            p++;
            continue;
        }
        if (*p == '&') {
            char c;
            p = decode_entity(p, &c);
            if (c == '\n' || c == '\r') {
                if (nl_run < 2) {
                    append_ch(&dst, &len, &cap, '\n');
                    nl_run++;
                }
            } else if (c == ' ' || c == '\t') {
                if (len && dst[len - 1] != ' ' && dst[len - 1] != '\n') {
                    append_ch(&dst, &len, &cap, ' ');
                }
                nl_run = 0;
            } else {
                append_ch(&dst, &len, &cap, c);
                nl_run = 0;
            }
            continue;
        }
        if (*p == '\r') {
            p++;
            continue;
        }
        if (*p == '\n') {
            if (nl_run < 2) {
                append_ch(&dst, &len, &cap, '\n');
                nl_run++;
            }
            p++;
            continue;
        }
        if (*p == '\t') {
            if (len && dst[len - 1] != ' ' && dst[len - 1] != '\n') {
                append_ch(&dst, &len, &cap, ' ');
            }
            nl_run = 0;
            p++;
            continue;
        }
        append_ch(&dst, &len, &cap, *p++);
        nl_run = 0;
    }
    if (dst) {
        dst[len] = 0;
    } else {
        dst = heap_caps_malloc(1, os_mem_bulk_caps());
        if (dst) {
            dst[0] = 0;
        }
        len = 0;
    }
    *out = dst;
    *out_len = len;
}

// ---- fetch worker ---------------------------------------------------------

static void fail_fetch(const char *msg)
{
    strlcpy(s_err, msg, sizeof(s_err));
    s_fetch = FETCH_FAIL;
}

static void worker(void *arg)
{
    (void)arg;
    char url[URL_MAX];
    strlcpy(url, s_url, sizeof(url));
    normalize_url(url, sizeof(url));
    if (!url[0]) {
        fail_fetch("No URL");
        vTaskDelete(NULL);
        return;
    }

    char *html = heap_caps_malloc(HTML_MAX, os_mem_bulk_caps());
    if (!html) {
        fail_fetch("Out of memory");
        vTaskDelete(NULL);
        return;
    }
    html[0] = 0;
    int status = 0;
    esp_err_t err = os_http_request("GET", url, NULL, NULL, html, HTML_MAX, &status);
    s_http_status = status;
    if (err != ESP_OK) {
        free(html);
        snprintf(s_err, sizeof(s_err), "HTTP err %s", esp_err_to_name(err));
        s_fetch = FETCH_FAIL;
        vTaskDelete(NULL);
        return;
    }
    if (status < 200 || status >= 400) {
        // Still try to show a body when present (e.g. 404 page text).
        if (!html[0]) {
            free(html);
            snprintf(s_err, sizeof(s_err), "HTTP %d", status);
            s_fetch = FETCH_FAIL;
            vTaskDelete(NULL);
            return;
        }
    }

    char *plain = NULL;
    size_t plen = 0;
    if (strchr(html, '<')) {
        html_to_text(html, &plain, &plen);
        free(html);
    } else {
        // Plain response: keep as-is, capped.
        size_t n = strlen(html);
        if (n > TEXT_MAX) {
            n = TEXT_MAX;
            html[n] = 0;
        }
        plain = html;
        plen = n;
    }
    if (!plain) {
        fail_fetch("Strip failed");
        vTaskDelete(NULL);
        return;
    }
    if (!plen) {
        free(plain);
        fail_fetch("Empty page");
        vTaskDelete(NULL);
        return;
    }

    free(s_pending_text);
    s_pending_text = plain;
    s_pending_len = plen;
    strlcpy(s_pending_url, url, sizeof(s_pending_url));
    strlcpy(s_url, url, sizeof(s_url));
    nvs_set_url(s_url);
    s_err[0] = 0;
    s_fetch = FETCH_OK;
    ESP_LOGI(TAG, "fetched %s (%d) → %u text bytes", url, status, (unsigned)plen);
    vTaskDelete(NULL);
}

static bool start_fetch(void)
{
    if (s_fetch == FETCH_BUSY) {
        return false;
    }
    if (!wifi_ready()) {
        os_toast("Wi-Fi needed");
        return false;
    }
    char url[URL_MAX];
    strlcpy(url, s_url, sizeof(url));
    normalize_url(url, sizeof(url));
    if (!url[0]) {
        os_toast("Enter a URL first");
        return false;
    }
    strlcpy(s_url, url, sizeof(s_url));
    s_err[0] = 0;
    s_http_status = 0;
    s_fetch = FETCH_BUSY;
    if (xTaskCreate(worker, "browser", WORKER_STACK, NULL, 3, NULL) != pdPASS) {
        s_fetch = FETCH_FAIL;
        strlcpy(s_err, "Task create failed", sizeof(s_err));
        return false;
    }
    return true;
}

// ---- UI -------------------------------------------------------------------

static void clear_reader(void)
{
    if (s_scroll) {
        lv_obj_delete(s_scroll);
        s_scroll = NULL;
    }
    s_body = s_title = NULL;
}

static void clear_home(void)
{
    if (s_home_title) {
        lv_obj_delete(s_home_title);
        s_home_title = NULL;
    }
    if (s_home_url) {
        lv_obj_delete(s_home_url);
        s_home_url = NULL;
    }
    if (s_home_st) {
        lv_obj_delete(s_home_st);
        s_home_st = NULL;
    }
}

static void set_hint(const char *text)
{
    if (!s_hint) {
        s_hint = ui_hint(s_root, text);
    } else {
        lv_label_set_text(s_hint, text);
    }
}

static void show_home(void)
{
    s_view = V_HOME;
    clear_reader();
    clear_home();

    s_home_title = ui_label(s_root, &sol_font_20, COL_TEXT, "Browser");
    lv_obj_set_pos(s_home_title, 10, 10);

    s_home_url = ui_label(s_root, &sol_mono_12, COL_DIM, s_url[0] ? s_url : "(no URL yet)");
    lv_obj_set_pos(s_home_url, 10, 48);
    lv_obj_set_width(s_home_url, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_home_url, LV_LABEL_LONG_MODE_WRAP);

    const char *st = "SELECT go   LEFT type URL   CANCEL exit";
    if (s_fetch == FETCH_BUSY) {
        st = "Fetching…";
    } else if (s_fetch == FETCH_FAIL && s_err[0]) {
        st = s_err;
    }
    s_home_st = ui_label(s_root, &sol_font_14, s_fetch == FETCH_FAIL ? COL_ORANGE : COL_TEXT, st);
    lv_obj_set_pos(s_home_st, 10, 120);
    lv_obj_set_width(s_home_st, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_home_st, LV_LABEL_LONG_MODE_WRAP);

    set_hint("SELECT go  LEFT url  CANCEL exit");
    if (s_offline) {
        lv_obj_move_foreground(s_offline);
    }
}

static void show_reader(void)
{
    s_view = V_READ;
    clear_home();
    clear_reader();

    s_scroll = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, OS_ROOT_W, CONTENT_H);
    lv_obj_set_style_pad_hor(s_scroll, 10, 0);
    lv_obj_set_style_pad_ver(s_scroll, 6, 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_ACTIVE);

    char head[URL_MAX + 16];
    if (s_http_status > 0) {
        snprintf(head, sizeof(head), "%d  %s", s_http_status, s_url);
    } else {
        strlcpy(head, s_url, sizeof(head));
    }
    s_title = ui_label(s_scroll, &sol_mono_12, COL_DIM, head);
    lv_obj_set_width(s_title, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    s_body = ui_label(s_scroll, &sol_font_14, COL_TEXT, s_text ? s_text : "");
    lv_obj_set_width(s_body, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_MODE_WRAP);

    set_hint(LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll  SELECT url  CANCEL back");
    if (s_offline) {
        lv_obj_move_foreground(s_offline);
    }
}

static void open_keyboard(void)
{
    if (s_kb) {
        return;
    }
    // ui_kb caps at 64 chars; longer URLs via console `browser url …`.
    s_kb = ui_kb_create(s_root, "URL (https://…)", 63);
}

static void apply_pending(void)
{
    if (s_fetch != FETCH_OK && s_fetch != FETCH_FAIL) {
        return;
    }
    if (s_fetch == FETCH_FAIL) {
        if (s_view != V_HOME) {
            show_home();
        } else if (s_home_st) {
            lv_label_set_text(s_home_st, s_err[0] ? s_err : "Fetch failed");
            lv_obj_set_style_text_color(s_home_st, ui_col(COL_ORANGE), 0);
        }
        os_toast("%s", s_err[0] ? s_err : "Fetch failed");
        s_fetch = FETCH_IDLE;
        return;
    }

    free(s_text);
    s_text = s_pending_text;
    s_text_len = s_pending_len;
    s_pending_text = NULL;
    s_pending_len = 0;
    if (s_pending_url[0]) {
        strlcpy(s_url, s_pending_url, sizeof(s_url));
        s_pending_url[0] = 0;
    }
    s_fetch = FETCH_IDLE;
    show_reader();
}

// ---- console --------------------------------------------------------------

static int cmd_browser(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "status")) {
        printf("url %s\n", s_url[0] ? s_url : "(none)");
        printf("view %s fetch %d status %d text %u\n",
               s_view == V_READ ? "read" : "home", (int)s_fetch, s_http_status,
               (unsigned)s_text_len);
        if (s_err[0]) {
            printf("err %s\n", s_err);
        }
        return 0;
    }
    if (!strcmp(argv[1], "url")) {
        if (argc < 3) {
            printf("usage: browser url <http(s)://...|clear>\n");
            return 1;
        }
        if (!strcmp(argv[2], "clear")) {
            s_url[0] = 0;
            nvs_set_url("");
            printf("cleared\n");
            return 0;
        }
        // Join remaining args for spaces (rare).
        char buf[URL_MAX];
        buf[0] = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2) {
                strlcat(buf, " ", sizeof(buf));
            }
            strlcat(buf, argv[i], sizeof(buf));
        }
        normalize_url(buf, sizeof(buf));
        strlcpy(s_url, buf, sizeof(s_url));
        nvs_set_url(s_url);
        printf("url %s\n", s_url);
        return 0;
    }
    if (!strcmp(argv[1], "go")) {
        if (!start_fetch()) {
            return 1;
        }
        printf("fetching %s\n", s_url);
        return 0;
    }
    if (!strcmp(argv[1], "clear")) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        s_url[0] = 0;
        printf("cleared NVS browser config\n");
        return 0;
    }
    printf("browser [status|url <url|clear>|go|clear]\n");
    return 1;
}

static void browser_register_cmd(void)
{
    static bool once;
    if (once) {
        return;
    }
    once = true;
    os_cmd_register("browser",
                    "browser [status|url <url|clear>|go|clear] - Wi-Fi text browser",
                    cmd_browser);
}

__attribute__((constructor)) static void browser_ctor(void)
{
    browser_register_cmd();
}

// ---- app lifecycle --------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_kb = NULL;
    s_hint = NULL;
    s_scroll = s_title = s_body = NULL;
    s_home_title = s_home_url = s_home_st = NULL;
    s_text = NULL;
    s_text_len = 0;
    s_pending_text = NULL;
    s_pending_len = 0;
    s_pending_url[0] = 0;
    s_err[0] = 0;
    s_http_status = 0;
    s_fetch = FETCH_IDLE;
    load_url();

    s_offline = ui_offline_create(root, 0, CONTENT_H, false);
    show_home();
}

static void leave(void)
{
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    free(s_pending_text);
    s_pending_text = NULL;
    s_pending_len = 0;
    s_root = s_hint = s_offline = NULL;
    s_home_title = s_home_url = s_home_st = NULL;
    s_scroll = s_title = s_body = NULL;
}

static void tick(void)
{
    if (!s_root) {
        return;
    }
    ui_offline_update(s_offline);
    if (wifi_ready() && s_offline) {
        lv_obj_set_hidden(s_offline, true);
    }
    if (s_fetch == FETCH_BUSY && s_view == V_HOME && s_home_st) {
        lv_label_set_text(s_home_st, "Fetching…");
        lv_obj_set_style_text_color(s_home_st, ui_col(COL_TEXT), 0);
    }
    if (s_fetch == FETCH_OK || s_fetch == FETCH_FAIL) {
        apply_pending();
    }
}

static bool key(os_key_t k, bool repeat)
{
    if (s_kb) {
        switch (ui_kb_key(s_kb, k, repeat)) {
        case UI_KB_DONE: {
            char buf[URL_MAX];
            strlcpy(buf, ui_kb_text(s_kb), sizeof(buf));
            ui_kb_delete(s_kb);
            s_kb = NULL;
            normalize_url(buf, sizeof(buf));
            strlcpy(s_url, buf, sizeof(s_url));
            nvs_set_url(s_url);
            if (s_view == V_HOME) {
                show_home();
            }
            if (s_url[0] && wifi_ready()) {
                start_fetch();
                if (s_view == V_HOME) {
                    show_home();
                }
            } else if (!s_url[0]) {
                os_toast("Empty URL");
            }
            return true;
        }
        case UI_KB_CANCEL:
            ui_kb_delete(s_kb);
            s_kb = NULL;
            return true;
        default:
            return true;
        }
    }

    if (ui_offline_key(s_offline, k)) {
        return true;
    }
    if (s_offline && !lv_obj_is_hidden(s_offline)) {
        return k != OS_KEY_BACK;
    }

    if (s_view == V_READ) {
        if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
            if (s_scroll) {
                lv_obj_scroll_by_bounded(s_scroll, 0,
                                         k == OS_KEY_DOWN ? -SCROLL_STEP : SCROLL_STEP, LV_ANIM_ON);
            }
            return true;
        }
        if (k == OS_KEY_SELECT && !repeat) {
            open_keyboard();
            return true;
        }
        if (k == OS_KEY_BACK) {
            show_home();
            return true;
        }
        return false;
    }

    // Home
    if (k == OS_KEY_LEFT && !repeat && s_fetch != FETCH_BUSY) {
        open_keyboard();
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        if (s_fetch == FETCH_BUSY) {
            return true;
        }
        if (!s_url[0]) {
            open_keyboard();
            return true;
        }
        start_fetch();
        show_home();
        return true;
    }
    return false;
}

const os_app_t app_browser = {
    .id = "browser",
    .name = "Browser",
    .symbol = LV_SYMBOL_HOME,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
