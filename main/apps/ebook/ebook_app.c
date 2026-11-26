// Ebook reader: plain .txt and simplified EPUB (ZIP + HTML) from microSD
// (/sdcard/books or /sdcard/ebooks). D-pad scrolls, LEFT/RIGHT change font
// size, last path + scroll + font persist in NVS.

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bsp.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"
#include "ebook_zip.h"

static const char *TAG = "ebook";

#define BOOKS_DIR_A     BSP_SD_MOUNT "/books"
#define BOOKS_DIR_B     BSP_SD_MOUNT "/ebooks"
#define MAX_BOOKS       48
#define PATH_MAX_LEN    200
#define TEXT_MAX        (384 * 1024)
#define SCROLL_STEP     40
#define NVS_NS          "ebook"
#define VIEW_H          (OS_ROOT_H - UI_HINT_H)

enum { FONT_SM = 0, FONT_MD, FONT_LG, FONT_COUNT };
static const lv_font_t *const k_fonts[FONT_COUNT] = {
    &sol_mono_12,
    &sol_font_14,
    &sol_font_20,
};
static const char *const k_font_names[FONT_COUNT] = { "12", "14", "20" };

typedef enum { V_PICKER, V_READ } view_t;

static lv_obj_t *s_root, *s_hint, *s_msg, *s_scroll, *s_body, *s_title;
static ui_list_t *s_list;
static view_t s_view;
static int s_font;
static char *s_text;
static size_t s_text_len;
static char s_path[PATH_MAX_LEN];
static EXT_RAM_BSS_ATTR char s_books[MAX_BOOKS][PATH_MAX_LEN];
static int s_nbooks;
static int32_t s_restore_y = -1;

static void show_picker(void);
static void open_book(const char *path);
static void close_reader(void);
static void save_pos(void);
static void load_pos(char *path_out, size_t path_n, int *font, int32_t *scroll_y);

static bool is_book(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".epub"));
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && s_nbooks < MAX_BOOKS) {
        size_t dl = strlen(dir), nl = strlen(de->d_name);
        if (!is_book(de->d_name) || dl + 1 + nl >= PATH_MAX_LEN) {
            continue;
        }
        char *p = s_books[s_nbooks++];
        memcpy(p, dir, dl);
        p[dl] = '/';
        memcpy(p + dl + 1, de->d_name, nl + 1);
    }
    closedir(d);
}

static void book_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    const char *slash = strrchr(s_books[i], '/');
    const char *name = slash ? slash + 1 : s_books[i];
    strlcpy(row->title, name, sizeof(row->title));
    const char *dot = strrchr(name, '.');
    if (dot && !strcasecmp(dot, ".epub")) {
        strlcpy(row->sub, "EPUB", sizeof(row->sub));
        strlcpy(row->right, "epub", sizeof(row->right));
    } else {
        strlcpy(row->sub, "Text", sizeof(row->sub));
        strlcpy(row->right, "txt", sizeof(row->right));
    }
    row->right_color = COL_DIM;
}

static void message(const char *text)
{
    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    s_msg = ui_label(s_root, &sol_font_14, COL_TEXT, text);
    lv_obj_set_width(s_msg, OS_ROOT_W - 32);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, -8);
}

static void clear_picker_ui(void)
{
    if (s_list) {
        // ui_list lives under root; hide then drop the pointer — root delete on
        // exit cleans objects. Rebuild by clearing children via new enter path.
        ui_list_show(s_list, false);
        s_list = NULL;
    }
    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    if (s_hint) {
        lv_obj_delete(s_hint);
        s_hint = NULL;
    }
}

// ---- HTML → plain text ----------------------------------------------------

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
    return !strncasecmp(tag, name, n) && (tag[n] == 0 || tag[n] == ' ' || tag[n] == '/' ||
                                          tag[n] == '\t' || tag[n] == '\n' || tag[n] == '\r');
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
                    tag_is(tag, "chapter")) {
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

static void text_append(char **dst, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (!s || !n || *len >= TEXT_MAX) {
        return;
    }
    if (n > TEXT_MAX - *len) {
        n = TEXT_MAX - *len;
    }
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 8192;
        while (ncap < *len + n + 1 && ncap < TEXT_MAX + 1) {
            ncap *= 2;
        }
        if (ncap > TEXT_MAX + 1) {
            ncap = TEXT_MAX + 1;
        }
        char *nb = heap_caps_realloc(*dst, ncap, os_mem_bulk_caps());
        if (!nb) {
            return;
        }
        *dst = nb;
        *cap = ncap;
    }
    memcpy(*dst + *len, s, n);
    *len += n;
    (*dst)[*len] = 0;
}

// ---- EPUB load ------------------------------------------------------------

typedef struct {
    char hrefs[64][160];
    char ids[64][64];
    int n_manifest;
    char spine[64][64];
    int n_spine;
    char root_dir[160];
} opf_t;

static void xml_attr(const char *tag, const char *key, char *out, size_t out_n)
{
    out[0] = 0;
    char pat[48];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *a = strstr(tag, pat);
    if (!a) {
        snprintf(pat, sizeof(pat), "%s='", key);
        a = strstr(tag, pat);
        if (!a) {
            return;
        }
    }
    a += strlen(pat);
    const char *end = strchr(a, pat[strlen(pat) - 1] == '"' ? '"' : '\'');
    // after key= the quote char is the last of pat
    char q = *(a - 1);
    end = strchr(a, q);
    if (!end) {
        return;
    }
    size_t n = (size_t)(end - a);
    if (n >= out_n) {
        n = out_n - 1;
    }
    memcpy(out, a, n);
    out[n] = 0;
}

static void parse_opf(const char *opf, const char *opf_path, opf_t *o)
{
    memset(o, 0, sizeof(*o));
    // Directory of the OPF inside the ZIP (for resolving relative hrefs).
    const char *slash = strrchr(opf_path, '/');
    if (slash) {
        size_t n = (size_t)(slash - opf_path) + 1;
        if (n < sizeof(o->root_dir)) {
            memcpy(o->root_dir, opf_path, n);
            o->root_dir[n] = 0;
        }
    }
    for (const char *p = opf; (p = strstr(p, "<item")) != NULL && o->n_manifest < 64;) {
        const char *end = strchr(p, '>');
        if (!end) {
            break;
        }
        char tag[512];
        size_t n = (size_t)(end - p) + 1;
        if (n >= sizeof(tag)) {
            n = sizeof(tag) - 1;
        }
        memcpy(tag, p, n);
        tag[n] = 0;
        char id[64], href[160], mt[80];
        xml_attr(tag, "id", id, sizeof(id));
        xml_attr(tag, "href", href, sizeof(href));
        xml_attr(tag, "media-type", mt, sizeof(mt));
        if (id[0] && href[0] &&
            (strstr(mt, "html") || strstr(mt, "XML") || strstr(mt, "xml") || !mt[0])) {
            // Prefer XHTML/HTML; still accept empty media-type with .htm* href.
            const char *dot = strrchr(href, '.');
            bool htmlish = (dot && (!strcasecmp(dot, ".xhtml") || !strcasecmp(dot, ".html") ||
                                    !strcasecmp(dot, ".htm") || !strcasecmp(dot, ".xml"))) ||
                           strstr(mt, "html") || strstr(mt, "xhtml");
            if (htmlish) {
                strlcpy(o->ids[o->n_manifest], id, sizeof(o->ids[0]));
                strlcpy(o->hrefs[o->n_manifest], href, sizeof(o->hrefs[0]));
                o->n_manifest++;
            }
        }
        p = end + 1;
    }
    for (const char *p = opf; (p = strstr(p, "<itemref")) != NULL && o->n_spine < 64;) {
        const char *end = strchr(p, '>');
        if (!end) {
            break;
        }
        char tag[256];
        size_t n = (size_t)(end - p) + 1;
        if (n >= sizeof(tag)) {
            n = sizeof(tag) - 1;
        }
        memcpy(tag, p, n);
        tag[n] = 0;
        char idref[64];
        xml_attr(tag, "idref", idref, sizeof(idref));
        if (idref[0]) {
            strlcpy(o->spine[o->n_spine++], idref, sizeof(o->spine[0]));
        }
        p = end + 1;
    }
}

static void join_zip_path(char *out, size_t n, const char *root, const char *href)
{
    // Resolve a single ".." segment; EPUB hrefs are usually flat under OEBPS/.
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s%s", root, href);
    // Collapse "/./" and simple "dir/../"
    char *w = out;
    const char *r = tmp;
    size_t left = n - 1;
    while (*r && left) {
        if (r[0] == '.' && r[1] == '/' ) {
            r += 2;
            continue;
        }
        if (r[0] == '.' && r[1] == '.' && r[2] == '/') {
            // back up one segment in out
            if (w > out) {
                w--;
                while (w > out && w[-1] != '/') {
                    w--;
                }
            }
            r += 3;
            left = n - 1 - (size_t)(w - out);
            continue;
        }
        *w++ = *r++;
        left--;
    }
    *w = 0;
}

static bool load_txt(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return false;
    }
    if (sz > TEXT_MAX) {
        sz = TEXT_MAX;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    char *buf = heap_caps_malloc((size_t)sz + 1, os_mem_bulk_caps());
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    // Strip UTF-8 BOM.
    if (n >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) {
        memmove(buf, buf + 3, n - 3);
        n -= 3;
        buf[n] = 0;
    }
    s_text = buf;
    s_text_len = n;
    return true;
}

static bool load_epub(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t *container = NULL;
    size_t cn = ebook_zip_extract(f, "META-INF/container.xml", &container);
    if (!cn || !container) {
        fclose(f);
        return false;
    }
    // full-path="..."
    char opf_path[160] = {0};
    const char *fp = strstr((char *)container, "full-path=\"");
    if (fp) {
        fp += 11;
        const char *e = strchr(fp, '"');
        if (e) {
            size_t n = (size_t)(e - fp);
            if (n < sizeof(opf_path)) {
                memcpy(opf_path, fp, n);
                opf_path[n] = 0;
            }
        }
    }
    free(container);
    if (!opf_path[0]) {
        fclose(f);
        return false;
    }

    uint8_t *opf = NULL;
    size_t on = ebook_zip_extract(f, opf_path, &opf);
    if (!on || !opf) {
        fclose(f);
        return false;
    }
    opf_t meta;
    parse_opf((char *)opf, opf_path, &meta);
    free(opf);

    char *text = NULL;
    size_t tlen = 0, tcap = 0;

    // Prefer spine order; fall back to all HTML-ish manifest items.
    int chapters = meta.n_spine > 0 ? meta.n_spine : meta.n_manifest;
    for (int i = 0; i < chapters && tlen < TEXT_MAX; i++) {
        const char *href = NULL;
        if (meta.n_spine > 0) {
            for (int m = 0; m < meta.n_manifest; m++) {
                if (!strcmp(meta.ids[m], meta.spine[i])) {
                    href = meta.hrefs[m];
                    break;
                }
            }
        } else {
            href = meta.hrefs[i];
        }
        if (!href) {
            continue;
        }
        char zpath[200];
        join_zip_path(zpath, sizeof(zpath), meta.root_dir, href);
        uint8_t *html = NULL;
        size_t hn = ebook_zip_extract(f, zpath, &html);
        if (!hn || !html) {
            continue;
        }
        char *plain = NULL;
        size_t plen = 0;
        html_to_text((char *)html, &plain, &plen);
        free(html);
        if (plain && plen) {
            if (tlen) {
                text_append(&text, &tlen, &tcap, "\n\n", 2);
            }
            text_append(&text, &tlen, &tcap, plain, plen);
        }
        free(plain);
        vTaskDelay(1);
    }
    fclose(f);

    if (!text || !tlen) {
        free(text);
        return false;
    }
    s_text = text;
    s_text_len = tlen;
    return true;
}

// ---- NVS ------------------------------------------------------------------

static void save_pos(void)
{
    if (!s_path[0]) {
        return;
    }
    int32_t y = 0;
    if (s_scroll) {
        y = lv_obj_get_scroll_y(s_scroll);
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "path", s_path);
    nvs_set_i32(h, "scroll", y);
    nvs_set_i32(h, "font", s_font);
    nvs_commit(h);
    nvs_close(h);
}

static void load_pos(char *path_out, size_t path_n, int *font, int32_t *scroll_y)
{
    path_out[0] = 0;
    *font = FONT_MD;
    *scroll_y = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = path_n;
    nvs_get_str(h, "path", path_out, &n);
    int32_t v;
    if (nvs_get_i32(h, "font", &v) == ESP_OK && v >= 0 && v < FONT_COUNT) {
        *font = (int)v;
    }
    if (nvs_get_i32(h, "scroll", &v) == ESP_OK && v >= 0) {
        *scroll_y = v;
    }
    nvs_close(h);
}

// ---- Reader UI ------------------------------------------------------------

static void set_reader_hint(void)
{
    if (!s_hint) {
        return;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll  "
             LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " font %s  CANCEL back",
             k_font_names[s_font]);
    lv_label_set_text(s_hint, buf);
}

static void apply_font(void)
{
    if (!s_body) {
        return;
    }
    lv_obj_set_style_text_font(s_body, k_fonts[s_font], 0);
    if (s_title) {
        lv_obj_set_style_text_font(s_title, s_font == FONT_LG ? &sol_font_14 : &sol_mono_12, 0);
    }
    set_reader_hint();
}

static void show_reader(void)
{
    s_view = V_READ;
    clear_picker_ui();

    const char *slash = strrchr(s_path, '/');
    const char *name = slash ? slash + 1 : s_path;

    s_scroll = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, OS_ROOT_W, VIEW_H);
    lv_obj_set_style_pad_hor(s_scroll, 10, 0);
    lv_obj_set_style_pad_ver(s_scroll, 6, 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_ACTIVE);

    s_title = ui_label(s_scroll, &sol_mono_12, COL_DIM, name);
    lv_obj_set_width(s_title, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    s_body = ui_label(s_scroll, k_fonts[s_font], COL_TEXT, s_text ? s_text : "");
    lv_obj_set_width(s_body, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_MODE_WRAP);

    s_hint = ui_hint(s_root, "");
    set_reader_hint();
    apply_font();

    if (s_restore_y > 0) {
        lv_obj_scroll_to_y(s_scroll, s_restore_y, LV_ANIM_OFF);
        s_restore_y = -1;
    }
}

static void close_reader(void)
{
    save_pos();
    if (s_scroll) {
        lv_obj_delete(s_scroll);
        s_scroll = NULL;
    }
    s_body = s_title = NULL;
    if (s_hint) {
        lv_obj_delete(s_hint);
        s_hint = NULL;
    }
    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    s_view = V_PICKER;
    show_picker();
}

static void open_book(const char *path)
{
    clear_picker_ui();
    message("Loading…");
    lv_refr_now(NULL);

    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    strlcpy(s_path, path, sizeof(s_path));

    const char *dot = strrchr(path, '.');
    bool ok = false;
    if (dot && !strcasecmp(dot, ".epub")) {
        ok = load_epub(path);
    } else {
        ok = load_txt(path);
    }

    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    if (!ok) {
        os_toast("Couldn't open book");
        show_picker();
        return;
    }
    ESP_LOGI(TAG, "opened %s (%u bytes text)", path, (unsigned)s_text_len);
    show_reader();
    save_pos();
}

static void show_picker(void)
{
    s_view = V_PICKER;
    s_list = NULL;
    s_msg = s_hint = NULL;
    s_nbooks = 0;

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nPut .txt / .epub in /books or /ebooks.");
        s_hint = ui_hint(s_root, "CANCEL exit");
        return;
    }
    scan_dir(BOOKS_DIR_A);
    scan_dir(BOOKS_DIR_B);
    ESP_LOGI(TAG, "picker %d books", s_nbooks);
    if (!s_nbooks) {
        message("No books found.\nCopy .txt or .epub to /books or /ebooks.");
        s_hint = ui_hint(s_root, "CANCEL exit");
        return;
    }
    s_list = ui_list_create(s_root, 0, OS_ROOT_H - UI_HINT_H, book_row, NULL);
    ui_list_set_count(s_list, s_nbooks);

    // Highlight last book if present.
    char last[PATH_MAX_LEN];
    int font;
    int32_t y;
    load_pos(last, sizeof(last), &font, &y);
    s_font = font;
    for (int i = 0; i < s_nbooks; i++) {
        if (!strcmp(s_books[i], last)) {
            ui_list_select(s_list, i);
            break;
        }
    }
    s_hint = ui_hint(s_root, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT open   CANCEL exit");
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_view = V_PICKER;
    s_scroll = s_body = s_title = s_hint = s_msg = NULL;
    s_list = NULL;
    s_text = NULL;
    s_text_len = 0;
    s_path[0] = 0;
    s_restore_y = -1;

    char last[PATH_MAX_LEN];
    int font;
    int32_t y;
    load_pos(last, sizeof(last), &font, &y);
    s_font = font;

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state == BSP_SD_MOUNTED && last[0] && access(last, R_OK) == 0) {
        s_restore_y = y;
        open_book(last);
        return;
    }
    show_picker();
}

static void exit_app(void)
{
    if (s_view == V_READ) {
        save_pos();
    }
    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    s_root = s_hint = s_msg = s_scroll = s_body = s_title = NULL;
    s_list = NULL;
    s_path[0] = 0;
}

static bool on_key(os_key_t key, bool repeat)
{
    if (s_view == V_PICKER) {
        if (s_list && ui_list_key(s_list, key, repeat)) {
            return true;
        }
        if (key == OS_KEY_SELECT && s_list) {
            int i = ui_list_selected(s_list);
            if (i >= 0 && i < s_nbooks) {
                s_restore_y = -1;
                // Restore scroll only when reopening the saved book.
                char last[PATH_MAX_LEN];
                int font;
                int32_t y;
                load_pos(last, sizeof(last), &font, &y);
                if (!strcmp(last, s_books[i])) {
                    s_font = font;
                    s_restore_y = y;
                }
                open_book(s_books[i]);
            }
            return true;
        }
        return false;   // CANCEL / HOME → exit
    }

    // Reader
    if (key == OS_KEY_UP || key == OS_KEY_DOWN) {
        if (s_scroll) {
            lv_obj_scroll_by_bounded(s_scroll, 0, key == OS_KEY_DOWN ? -SCROLL_STEP : SCROLL_STEP,
                                     LV_ANIM_ON);
            save_pos();
        }
        return true;
    }
    if (key == OS_KEY_LEFT || key == OS_KEY_RIGHT) {
        int32_t y = s_scroll ? lv_obj_get_scroll_y(s_scroll) : 0;
        int prev = s_font;
        if (key == OS_KEY_RIGHT) {
            s_font = (s_font + 1) % FONT_COUNT;
        } else {
            s_font = (s_font + FONT_COUNT - 1) % FONT_COUNT;
        }
        apply_font();
        // Roughly keep place when line height changes.
        static const int lh[FONT_COUNT] = { 14, 17, 24 };
        if (s_scroll && lh[prev]) {
            int32_t ny = (int32_t)((int64_t)y * lh[s_font] / lh[prev]);
            lv_obj_scroll_to_y(s_scroll, ny, LV_ANIM_OFF);
        }
        save_pos();
        return true;
    }
    if (key == OS_KEY_BACK) {
        close_reader();
        return true;
    }
    if (key == OS_KEY_SELECT) {
        // Page down on SELECT for one-handed reading.
        if (s_scroll) {
            lv_obj_scroll_by_bounded(s_scroll, 0, -(VIEW_H - 24), LV_ANIM_ON);
            save_pos();
        }
        return true;
    }
    return false;
}

const os_app_t app_ebook = {
    .id = "ebook",
    .name = "Ebook",
    .symbol = LV_SYMBOL_FILE,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = exit_app,
    .on_key = on_key,
};
