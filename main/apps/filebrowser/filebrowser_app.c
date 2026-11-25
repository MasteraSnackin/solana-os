// File Browser: list and navigate /sdcard on the BP26 badge. SELECT opens a
// directory or a plain .txt file; CANCEL goes up a folder (or exits at root /
// closes the text viewer). Non-text files toast a short message.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"

static const char *TAG = "files";

#define ROOT_DIR        BSP_SD_MOUNT
#define MAX_ENTRIES     96
#define NAME_MAX_LEN    64
#define PATH_MAX_LEN    200
#define TEXT_MAX        (128 * 1024)
#define SCROLL_STEP     40
#define VIEW_H          (OS_ROOT_H - UI_HINT_H)

typedef enum { V_LIST, V_TEXT } view_t;

typedef struct {
    char name[NAME_MAX_LEN];
    bool is_dir;
    bool is_up;             // synthetic ".."
    int64_t size;
} entry_t;

static lv_obj_t *s_root, *s_hint, *s_msg, *s_path_lbl;
static lv_obj_t *s_scroll, *s_body, *s_title;
static ui_list_t *s_list;
static view_t s_view;

static char s_cwd[PATH_MAX_LEN];
static entry_t s_ents[MAX_ENTRIES];
static int s_nents;

static char *s_text;
static size_t s_text_len;
static char s_text_path[PATH_MAX_LEN];

static void show_list(void);
static void open_txt(const char *path);
static void close_text(void);

static bool is_txt(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".txt");
}

static bool path_join(char *out, size_t n, const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl >= n) {
        return false;
    }
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl + 1);
    return true;
}

static const char *display_cwd(void)
{
    // Show path relative to the mount point for a shorter breadcrumb.
    if (!strncmp(s_cwd, ROOT_DIR, strlen(ROOT_DIR))) {
        const char *rest = s_cwd + strlen(ROOT_DIR);
        return rest[0] ? rest : "/";
    }
    return s_cwd;
}

static int cmp_ent(const void *a, const void *b)
{
    const entry_t *ea = a, *eb = b;
    if (ea->is_up != eb->is_up) {
        return ea->is_up ? -1 : 1;
    }
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

static void scan_cwd(void)
{
    s_nents = 0;
    memset(s_ents, 0, sizeof(s_ents));

    // Parent entry when not at /sdcard.
    if (strcmp(s_cwd, ROOT_DIR) != 0 && s_nents < MAX_ENTRIES) {
        entry_t *e = &s_ents[s_nents++];
        strlcpy(e->name, "..", sizeof(e->name));
        e->is_dir = true;
        e->is_up = true;
        e->size = 0;
    }

    DIR *d = opendir(s_cwd);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed", s_cwd);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && s_nents < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        if (strlen(de->d_name) >= NAME_MAX_LEN) {
            continue;
        }
        char full[PATH_MAX_LEN];
        if (!path_join(full, sizeof(full), s_cwd, de->d_name)) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        entry_t *e = &s_ents[s_nents++];
        strlcpy(e->name, de->d_name, sizeof(e->name));
        e->is_dir = S_ISDIR(st.st_mode);
        e->is_up = false;
        e->size = e->is_dir ? 0 : (int64_t)st.st_size;
    }
    closedir(d);

    if (s_nents > 1) {
        // Keep ".." first; sort the rest.
        int start = s_ents[0].is_up ? 1 : 0;
        if (s_nents - start > 1) {
            qsort(s_ents + start, (size_t)(s_nents - start), sizeof(entry_t), cmp_ent);
        }
    }
    ESP_LOGI(TAG, "list %s (%d entries)", s_cwd, s_nents);
}

static void fmt_size(char *out, size_t n, int64_t sz)
{
    if (sz < 1024) {
        snprintf(out, n, "%lld B", (long long)sz);
    } else if (sz < 1024 * 1024) {
        snprintf(out, n, "%lld KB", (long long)((sz + 512) / 1024));
    } else {
        snprintf(out, n, "%lld MB", (long long)((sz + 512 * 1024) / (1024 * 1024)));
    }
}

static void row_cb(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    if (i < 0 || i >= s_nents) {
        return;
    }
    const entry_t *e = &s_ents[i];
    strlcpy(row->title, e->name, sizeof(row->title));
    if (e->is_up) {
        strlcpy(row->sub, "Parent folder", sizeof(row->sub));
        strlcpy(row->right, "up", sizeof(row->right));
        row->right_color = COL_DIM;
        row->title_color = COL_AQUA;
    } else if (e->is_dir) {
        strlcpy(row->sub, "Folder", sizeof(row->sub));
        strlcpy(row->right, "dir", sizeof(row->right));
        row->right_color = COL_AQUA;
        row->title_color = COL_AQUA;
    } else if (is_txt(e->name)) {
        fmt_size(row->sub, sizeof(row->sub), e->size);
        strlcpy(row->right, "txt", sizeof(row->right));
        row->right_color = COL_GREEN;
    } else {
        fmt_size(row->sub, sizeof(row->sub), e->size);
        strlcpy(row->right, "file", sizeof(row->right));
        row->right_color = COL_DIM;
    }
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

static void clear_list_ui(void)
{
    if (s_list) {
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
    if (s_path_lbl) {
        lv_obj_delete(s_path_lbl);
        s_path_lbl = NULL;
    }
}

static bool go_up(void)
{
    if (!strcmp(s_cwd, ROOT_DIR)) {
        return false;
    }
    char *slash = strrchr(s_cwd, '/');
    if (!slash || slash == s_cwd) {
        strlcpy(s_cwd, ROOT_DIR, sizeof(s_cwd));
        return true;
    }
    // Trim trailing segment; never climb above /sdcard.
    *slash = 0;
    if (strlen(s_cwd) < strlen(ROOT_DIR) || strncmp(s_cwd, ROOT_DIR, strlen(ROOT_DIR)) != 0) {
        strlcpy(s_cwd, ROOT_DIR, sizeof(s_cwd));
    }
    return true;
}

static void enter_dir(const char *name)
{
    if (!strcmp(name, "..")) {
        go_up();
        show_list();
        return;
    }
    char next[PATH_MAX_LEN];
    if (!path_join(next, sizeof(next), s_cwd, name)) {
        os_toast("Path too long");
        return;
    }
    DIR *d = opendir(next);
    if (!d) {
        os_toast("Can't open folder");
        return;
    }
    closedir(d);
    strlcpy(s_cwd, next, sizeof(s_cwd));
    show_list();
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
    if (n >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) {
        memmove(buf, buf + 3, n - 3);
        n -= 3;
        buf[n] = 0;
    }
    // Refuse obvious binary (NUL in the first 512 bytes).
    size_t check = n < 512 ? n : 512;
    for (size_t i = 0; i < check; i++) {
        if (buf[i] == 0) {
            free(buf);
            return false;
        }
    }
    s_text = buf;
    s_text_len = n;
    return true;
}

static void show_text(void)
{
    s_view = V_TEXT;
    clear_list_ui();

    const char *slash = strrchr(s_text_path, '/');
    const char *name = slash ? slash + 1 : s_text_path;

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

    s_body = ui_label(s_scroll, &sol_font_14, COL_TEXT, s_text ? s_text : "");
    lv_obj_set_width(s_body, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_MODE_WRAP);

    char hint[96];
    snprintf(hint, sizeof(hint), LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll   CANCEL back");
    if (s_text_len >= TEXT_MAX) {
        strlcat(hint, "  (truncated)", sizeof(hint));
    }
    s_hint = ui_hint(s_root, hint);
}

static void close_text(void)
{
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
    s_text_path[0] = 0;
    s_view = V_LIST;
    show_list();
}

static void open_txt(const char *path)
{
    clear_list_ui();
    message("Loading…");
    lv_refr_now(NULL);

    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    strlcpy(s_text_path, path, sizeof(s_text_path));

    bool ok = load_txt(path);
    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    if (!ok) {
        os_toast("Can't open as text");
        show_list();
        return;
    }
    ESP_LOGI(TAG, "opened %s (%u bytes)", path, (unsigned)s_text_len);
    show_text();
}

static void activate(int i)
{
    if (i < 0 || i >= s_nents) {
        return;
    }
    const entry_t *e = &s_ents[i];
    if (e->is_dir) {
        enter_dir(e->name);
        return;
    }
    if (!is_txt(e->name)) {
        os_toast("Open .txt only");
        return;
    }
    char full[PATH_MAX_LEN];
    if (!path_join(full, sizeof(full), s_cwd, e->name)) {
        os_toast("Path too long");
        return;
    }
    open_txt(full);
}

static void show_list(void)
{
    s_view = V_LIST;
    clear_list_ui();

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nInsert a card to browse /sdcard.");
        s_hint = ui_hint(s_root, "CANCEL exit");
        return;
    }

    scan_cwd();

    s_path_lbl = ui_label(s_root, &sol_mono_12, COL_DIM, "");
    lv_label_set_text_fmt(s_path_lbl, "%s", display_cwd());
    lv_obj_set_width(s_path_lbl, OS_ROOT_W - 12);
    lv_label_set_long_mode(s_path_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_path_lbl, LV_ALIGN_TOP_LEFT, 6, 2);

    if (!s_nents) {
        message("Empty folder.");
        s_hint = ui_hint(s_root, "CANCEL back");
        return;
    }

    const int path_h = 18;
    s_list = ui_list_create(s_root, path_h, OS_ROOT_H - UI_HINT_H - path_h, row_cb, NULL);
    ui_list_set_count(s_list, s_nents);
    s_hint = ui_hint(s_root,
                     LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT open   CANCEL back");
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_view = V_LIST;
    s_scroll = s_body = s_title = s_hint = s_msg = s_path_lbl = NULL;
    s_list = NULL;
    s_text = NULL;
    s_text_len = 0;
    s_text_path[0] = 0;
    s_nents = 0;
    strlcpy(s_cwd, ROOT_DIR, sizeof(s_cwd));
    show_list();
}

static void exit_app(void)
{
    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    s_root = s_hint = s_msg = s_path_lbl = s_scroll = s_body = s_title = NULL;
    s_list = NULL;
    s_cwd[0] = 0;
    s_text_path[0] = 0;
    s_nents = 0;
}

static bool on_key(os_key_t key, bool repeat)
{
    if (s_view == V_TEXT) {
        if (key == OS_KEY_UP || key == OS_KEY_DOWN) {
            if (s_scroll) {
                lv_obj_scroll_by_bounded(s_scroll, 0,
                                         key == OS_KEY_DOWN ? -SCROLL_STEP : SCROLL_STEP,
                                         LV_ANIM_ON);
            }
            return true;
        }
        if (key == OS_KEY_SELECT) {
            if (s_scroll) {
                lv_obj_scroll_by_bounded(s_scroll, 0, -(VIEW_H - 24), LV_ANIM_ON);
            }
            return true;
        }
        if (key == OS_KEY_BACK) {
            close_text();
            return true;
        }
        return false;
    }

    // List view
    if (s_list && ui_list_key(s_list, key, repeat)) {
        return true;
    }
    if (key == OS_KEY_SELECT && s_list) {
        activate(ui_list_selected(s_list));
        return true;
    }
    if (key == OS_KEY_BACK) {
        if (go_up()) {
            show_list();
            return true;
        }
        return false;   // at /sdcard → exit
    }
    return false;
}

const os_app_t app_filebrowser = {
    .id = "files",
    .name = "Files",
    .symbol = LV_SYMBOL_DIRECTORY,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = exit_app,
    .on_key = on_key,
};
