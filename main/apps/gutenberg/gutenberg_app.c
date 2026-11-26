// Gutenberg: browse a curated Project Gutenberg catalog, download plain-text
// UTF-8 books over Wi-Fi into /sdcard/gutenberg/, and read .txt from that
// folder or /sdcard/books/. Mirrors the Ebook reader UI (scroll, fonts, NVS).
// Does NOT ship any book body — Lewis Dartnell's "The Knowledge" is modern
// copyright and is not on Project Gutenberg; use public-domain PG texts only.

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bsp.h"
#include "core/os.h"
#include "core/os_http.h"
#include "core/os_mem.h"
#include "core/os_wifi.h"
#include "core/ui.h"

static const char *TAG = "gutenberg";

#define BOOKS_DIR_A     BSP_SD_MOUNT "/books"
#define BOOKS_DIR_B     BSP_SD_MOUNT "/gutenberg"
#define MAX_BOOKS       48
#define PATH_MAX_LEN    200
#define TEXT_MAX        (384 * 1024)
#define DL_MAX          TEXT_MAX
#define SCROLL_STEP     40
#define NVS_NS          "gutenberg"
#define VIEW_H          (OS_ROOT_H - UI_HINT_H)
#define WORKER_STACK    8192
#define URL_MAX         128
#define SEARCH_MAX      32
#define CTRL_SEARCH     0   // catalog list row 0: open on-screen search keyboard

enum { FONT_SM = 0, FONT_MD, FONT_LG, FONT_COUNT };
static const lv_font_t *const k_fonts[FONT_COUNT] = {
    &sol_mono_12,
    &sol_font_14,
    &sol_font_20,
};
static const char *const k_font_names[FONT_COUNT] = { "12", "14", "20" };

typedef enum { TAB_SD = 0, TAB_CATALOG, TAB_COUNT } tab_t;
typedef enum { V_PICKER, V_READ } view_t;
typedef enum { DL_IDLE = 0, DL_BUSY, DL_OK, DL_FAIL } dl_t;

typedef struct {
    int id;                     // Project Gutenberg ebook number
    const char *title;
    const char *author;
    const char *slug;           // filename stem under /gutenberg/
} pg_entry_t;

// Curated public-domain classics (plain-text UTF-8). No modern copyrighted works.
static const pg_entry_t k_catalog[] = {
    { 11,    "Alice in Wonderland",           "Lewis Carroll",          "alice" },
    { 12,    "Through the Looking-Glass",     "Lewis Carroll",          "looking-glass" },
    { 16,    "Peter Pan",                     "J. M. Barrie",           "peter-pan" },
    { 20,    "Paradise Lost",                 "John Milton",            "paradise-lost" },
    { 23,    "Narrative of Frederick Douglass", "Frederick Douglass",   "douglass" },
    { 33,    "The Scarlet Letter",            "Nathaniel Hawthorne",    "scarlet-letter" },
    { 35,    "The Time Machine",              "H. G. Wells",            "time-machine" },
    { 36,    "The War of the Worlds",         "H. G. Wells",            "war-worlds" },
    { 41,    "The Legend of Sleepy Hollow",   "Washington Irving",      "sleepy-hollow" },
    { 43,    "Dr. Jekyll and Mr. Hyde",       "R. L. Stevenson",        "jekyll-hyde" },
    { 45,    "Anne of Green Gables",          "L. M. Montgomery",       "anne-green" },
    { 46,    "A Christmas Carol",             "Charles Dickens",        "christmas-carol" },
    { 55,    "The Wonderful Wizard of Oz",    "L. Frank Baum",          "wizard-oz" },
    { 74,    "Tom Sawyer",                    "Mark Twain",             "tom-sawyer" },
    { 76,    "Huckleberry Finn",              "Mark Twain",             "huck-finn" },
    { 84,    "Frankenstein",                  "Mary Shelley",           "frankenstein" },
    { 98,    "A Tale of Two Cities",          "Charles Dickens",        "two-cities" },
    { 103,   "Around the World in 80 Days",   "Jules Verne",            "80-days" },
    { 105,   "Persuasion",                    "Jane Austen",            "persuasion" },
    { 120,   "Treasure Island",               "R. L. Stevenson",        "treasure-island" },
    { 121,   "Journey to the Center of the Earth", "Jules Verne",       "journey-earth" },
    { 158,   "Emma",                          "Jane Austen",            "emma" },
    { 161,   "Sense and Sensibility",         "Jane Austen",            "sense-sensibility" },
    { 164,   "Twenty Thousand Leagues",       "Jules Verne",            "20k-leagues" },
    { 174,   "The Picture of Dorian Gray",    "Oscar Wilde",            "dorian-gray" },
    { 203,   "Uncle Tom's Cabin",             "Harriet Beecher Stowe",  "uncle-tom" },
    { 205,   "Walden",                        "Henry David Thoreau",    "walden" },
    { 215,   "The Call of the Wild",          "Jack London",            "call-wild" },
    { 219,   "Heart of Darkness",             "Joseph Conrad",          "heart-darkness" },
    { 236,   "The Jungle Book",               "Rudyard Kipling",        "jungle-book" },
    { 244,   "A Study in Scarlet",            "Arthur Conan Doyle",     "study-scarlet" },
    { 345,   "Dracula",                       "Bram Stoker",            "dracula" },
    { 421,   "Kidnapped",                     "R. L. Stevenson",        "kidnapped" },
    { 514,   "Little Women",                  "Louisa May Alcott",      "little-women" },
    { 730,   "The Red Badge of Courage",      "Stephen Crane",          "red-badge" },
    { 768,   "Wuthering Heights",             "Emily Bronte",           "wuthering" },
    { 829,   "Gulliver's Travels",            "Jonathan Swift",         "gulliver" },
    { 844,   "The Importance of Being Earnest", "Oscar Wilde",          "earnest" },
    { 996,   "Don Quixote",                   "Miguel de Cervantes",    "don-quixote" },
    { 1080,  "A Modest Proposal",             "Jonathan Swift",         "modest-proposal" },
    { 1184,  "The Count of Monte Cristo",     "Alexandre Dumas",        "monte-cristo" },
    { 1232,  "The Prince",                    "Niccolo Machiavelli",    "the-prince" },
    { 1260,  "Jane Eyre",                     "Charlotte Bronte",       "jane-eyre" },
    { 1342,  "Pride and Prejudice",           "Jane Austen",            "pride-prejudice" },
    { 1399,  "Anna Karenina",                 "Leo Tolstoy",            "anna-karenina" },
    { 1400,  "Great Expectations",            "Charles Dickens",        "great-expectations" },
    { 1497,  "The Republic",                  "Plato",                  "republic" },
    { 1513,  "Romeo and Juliet",              "William Shakespeare",    "romeo-juliet" },
    { 1524,  "Hamlet",                        "William Shakespeare",    "hamlet" },
    { 1533,  "Macbeth",                       "William Shakespeare",    "macbeth" },
    { 1661,  "Sherlock Holmes (Adventures)",  "Arthur Conan Doyle",     "sherlock" },
    { 1727,  "The Odyssey",                   "Homer (Butler)",         "odyssey" },
    { 1952,  "The Yellow Wallpaper",          "Charlotte Perkins Gilman", "yellow-wallpaper" },
    { 2554,  "Crime and Punishment",          "Fyodor Dostoevsky",      "crime-punishment" },
    { 2600,  "War and Peace",                 "Leo Tolstoy",            "war-peace" },
    { 2701,  "Moby Dick",                     "Herman Melville",        "moby-dick" },
    { 2814,  "Dubliners",                     "James Joyce",            "dubliners" },
    { 2852,  "The Hound of the Baskervilles", "Arthur Conan Doyle",     "baskervilles" },
    { 5200,  "Metamorphosis",                 "Franz Kafka",            "metamorphosis" },
    { 6130,  "The Iliad",                     "Homer (Butler)",         "iliad" },
    { 16328, "Beowulf",                       "Anonymous",              "beowulf" },
    { 64317, "The Great Gatsby",              "F. Scott Fitzgerald",    "gatsby" },
};
static const int k_catalog_n = sizeof(k_catalog) / sizeof(k_catalog[0]);

static lv_obj_t *s_root, *s_hint, *s_msg, *s_scroll, *s_body, *s_title, *s_tab_lbl, *s_offline;
static ui_list_t *s_list;
static ui_kb_t *s_kb;
static view_t s_view;
static tab_t s_tab;
static int s_font;
static char *s_text;
static size_t s_text_len;
static char s_path[PATH_MAX_LEN];
static char s_books[MAX_BOOKS][PATH_MAX_LEN];
static int s_nbooks;
static int32_t s_restore_y = -1;

static char s_q[SEARCH_MAX];
static int s_filt[sizeof(k_catalog) / sizeof(k_catalog[0])];
static int s_nfilt;

static volatile dl_t s_dl;
static char s_err[96];
static int s_dl_catalog_i = -1;
static char s_pending_path[PATH_MAX_LEN];

static void show_picker(void);
static void open_book(const char *path);
static void save_pos(void);
static void load_pos(char *path_out, size_t path_n, int *font, int32_t *scroll_y);
static void rebuild_filter(void);

static bool wifi_ready(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED;
}

static bool is_txt(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !strcasecmp(dot, ".txt");
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
        if (!is_txt(de->d_name) || dl + 1 + nl >= PATH_MAX_LEN) {
            continue;
        }
        char *p = s_books[s_nbooks++];
        memcpy(p, dir, dl);
        p[dl] = '/';
        memcpy(p + dl + 1, de->d_name, nl + 1);
    }
    closedir(d);
}

static void catalog_path(char *out, size_t n, const pg_entry_t *e)
{
    snprintf(out, n, "%s/%d-%s.txt", BOOKS_DIR_B, e->id, e->slug);
}

static bool catalog_on_sd(const pg_entry_t *e, char *out, size_t n)
{
    catalog_path(out, n, e);
    return access(out, R_OK) == 0;
}

static void rebuild_filter(void)
{
    s_nfilt = 0;
    for (int i = 0; i < k_catalog_n; i++) {
        if (s_q[0]) {
            if (!strcasestr(k_catalog[i].title, s_q) &&
                !strcasestr(k_catalog[i].author, s_q) &&
                !strcasestr(k_catalog[i].slug, s_q)) {
                continue;
            }
        }
        s_filt[s_nfilt++] = i;
    }
}

static int catalog_list_count(void)
{
    return 1 + s_nfilt;   // search row + matches
}

static const pg_entry_t *catalog_at_list(int list_i)
{
    int fi = list_i - 1;
    if (fi < 0 || fi >= s_nfilt) {
        return NULL;
    }
    return &k_catalog[s_filt[fi]];
}

static void book_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    if (s_tab == TAB_CATALOG) {
        if (i == CTRL_SEARCH) {
            strlcpy(row->title, LV_SYMBOL_EYE_OPEN "  Search", sizeof(row->title));
            if (s_q[0]) {
                strlcpy(row->sub, s_q, sizeof(row->sub));
                snprintf(row->right, sizeof(row->right), "%d", s_nfilt);
                row->right_color = COL_OK;
            } else {
                strlcpy(row->sub, "Filter by title or author", sizeof(row->sub));
                strlcpy(row->right, "kb", sizeof(row->right));
                row->right_color = COL_DIM;
            }
            return;
        }
        const pg_entry_t *e = catalog_at_list(i);
        if (!e) {
            return;
        }
        strlcpy(row->title, e->title, sizeof(row->title));
        strlcpy(row->sub, e->author, sizeof(row->sub));
        char path[PATH_MAX_LEN];
        if (catalog_on_sd(e, path, sizeof(path))) {
            strlcpy(row->right, "on SD", sizeof(row->right));
            row->right_color = COL_OK;
        } else {
            snprintf(row->right, sizeof(row->right), "#%d", e->id);
            row->right_color = COL_DIM;
        }
        return;
    }
    const char *slash = strrchr(s_books[i], '/');
    const char *name = slash ? slash + 1 : s_books[i];
    strlcpy(row->title, name, sizeof(row->title));
    strlcpy(row->sub, "Text", sizeof(row->sub));
    strlcpy(row->right, "txt", sizeof(row->right));
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
    // Drop every root child except the offline panel (ui_list frees itself on
    // box DELETE). Avoids leaking a new list each tab switch. Do not call
    // while s_kb is open — delete the keyboard first.
    s_list = NULL;
    s_msg = s_hint = s_tab_lbl = NULL;
    if (!s_root) {
        return;
    }
    for (int i = (int)lv_obj_get_child_count(s_root) - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(s_root, i);
        if (c == s_offline) {
            continue;
        }
        lv_obj_delete(c);
    }
}

static bool ensure_gutenberg_dir(void)
{
    if (mkdir(BOOKS_DIR_B, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    return false;
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
    s_text = buf;
    s_text_len = n;
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
    nvs_set_i32(h, "tab", (int32_t)s_tab);
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
    if (nvs_get_i32(h, "tab", &v) == ESP_OK && v >= 0 && v < TAB_COUNT) {
        s_tab = (tab_t)v;
    }
    nvs_close(h);
}

// ---- Download worker ------------------------------------------------------

static void fail_dl(const char *msg)
{
    strlcpy(s_err, msg, sizeof(s_err));
    s_dl = DL_FAIL;
}

static void worker(void *arg)
{
    (void)arg;
    int i = s_dl_catalog_i;
    if (i < 0 || i >= k_catalog_n) {
        fail_dl("Bad catalog");
        vTaskDelete(NULL);
        return;
    }
    const pg_entry_t *e = &k_catalog[i];
    if (!ensure_gutenberg_dir()) {
        fail_dl("mkdir failed");
        vTaskDelete(NULL);
        return;
    }

    char path[PATH_MAX_LEN];
    catalog_path(path, sizeof(path), e);
    char url[URL_MAX];
    snprintf(url, sizeof(url), "https://www.gutenberg.org/ebooks/%d.txt.utf-8", e->id);

    int status = 0;
    esp_err_t err = os_http_download(url, NULL, path, DL_MAX, NULL, NULL, 0, &status);
    if (err != ESP_OK) {
        unlink(path);
        snprintf(s_err, sizeof(s_err), "DL %s", esp_err_to_name(err));
        s_dl = DL_FAIL;
        vTaskDelete(NULL);
        return;
    }
    if (status < 200 || status >= 300) {
        unlink(path);
        snprintf(s_err, sizeof(s_err), "HTTP %d", status);
        s_dl = DL_FAIL;
        vTaskDelete(NULL);
        return;
    }
    strlcpy(s_pending_path, path, sizeof(s_pending_path));
    s_err[0] = 0;
    s_dl = DL_OK;
    ESP_LOGI(TAG, "downloaded #%d → %s", e->id, path);
    vTaskDelete(NULL);
}

static bool start_download(int catalog_i)
{
    if (s_dl == DL_BUSY) {
        return false;
    }
    if (!wifi_ready()) {
        os_toast("Wi-Fi needed");
        return false;
    }
    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        os_toast("Need microSD");
        return false;
    }
    s_dl_catalog_i = catalog_i;
    s_pending_path[0] = 0;
    s_err[0] = 0;
    s_dl = DL_BUSY;
    if (xTaskCreate(worker, "gut_dl", WORKER_STACK, NULL, 3, NULL) != pdPASS) {
        s_dl = DL_FAIL;
        strlcpy(s_err, "Task failed", sizeof(s_err));
        return false;
    }
    return true;
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
    if (s_offline) {
        lv_obj_set_hidden(s_offline, true);
    }

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
    s_scroll = s_body = s_title = NULL;
    clear_picker_ui();
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

    bool ok = load_txt(path);

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

static void set_picker_hint(void)
{
    if (!s_hint) {
        return;
    }
    if (s_dl == DL_BUSY) {
        lv_label_set_text(s_hint, "Downloading…  CANCEL exit");
        return;
    }
    if (s_tab == TAB_CATALOG) {
        lv_label_set_text(s_hint,
                          LV_SYMBOL_UP LV_SYMBOL_DOWN "  "
                          LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tab  SELECT get/search  CANCEL");
    } else {
        lv_label_set_text(s_hint,
                          LV_SYMBOL_UP LV_SYMBOL_DOWN "  "
                          LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " tab  SELECT open  CANCEL");
    }
}

static void show_picker(void)
{
    s_view = V_PICKER;
    s_list = NULL;
    s_msg = s_hint = s_tab_lbl = NULL;
    s_nbooks = 0;

    const char *tab_name = s_tab == TAB_CATALOG ? "Catalog" : "On SD";
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "Gutenberg — %s", tab_name);
    s_tab_lbl = ui_label(s_root, &sol_mono_12, COL_DIM, hdr);
    lv_obj_align(s_tab_lbl, LV_ALIGN_TOP_MID, 0, 2);

    int list_y = 16;
    int list_h = OS_ROOT_H - UI_HINT_H - list_y;

    if (s_tab == TAB_CATALOG) {
        if (s_dl == DL_BUSY) {
            message("Downloading from\nProject Gutenberg…");
            s_hint = ui_hint(s_root, "");
            set_picker_hint();
            return;
        }
        rebuild_filter();
        s_list = ui_list_create(s_root, list_y, list_h, book_row, NULL);
        ui_list_set_count(s_list, catalog_list_count());
        ui_list_set_empty(s_list, s_q[0] ? "No matches" : "Catalog empty");
        s_hint = ui_hint(s_root, "");
        set_picker_hint();
        return;
    }

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nPut .txt in /books or /gutenberg,\nor open Catalog to download.");
        s_hint = ui_hint(s_root, "");
        set_picker_hint();
        return;
    }
    scan_dir(BOOKS_DIR_A);
    scan_dir(BOOKS_DIR_B);
    ESP_LOGI(TAG, "picker %d books", s_nbooks);
    if (!s_nbooks) {
        message("No .txt on card.\nCopy into /books or /gutenberg,\nor Catalog + Wi-Fi to fetch.");
        s_hint = ui_hint(s_root, "");
        set_picker_hint();
        return;
    }
    s_list = ui_list_create(s_root, list_y, list_h, book_row, NULL);
    ui_list_set_count(s_list, s_nbooks);

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
    s_hint = ui_hint(s_root, "");
    set_picker_hint();
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_view = V_PICKER;
    s_tab = TAB_SD;
    s_scroll = s_body = s_title = s_hint = s_msg = s_tab_lbl = NULL;
    s_list = NULL;
    s_kb = NULL;
    s_text = NULL;
    s_text_len = 0;
    s_path[0] = 0;
    s_q[0] = 0;
    s_nfilt = 0;
    s_restore_y = -1;
    s_dl = DL_IDLE;
    s_err[0] = 0;
    s_pending_path[0] = 0;
    s_dl_catalog_i = -1;

    s_offline = ui_offline_create(root, 0, VIEW_H, false);

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
    // Wait briefly if a download is in flight so the worker does not touch freed UI.
    for (int i = 0; i < 50 && s_dl == DL_BUSY; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    s_root = s_hint = s_msg = s_scroll = s_body = s_title = s_tab_lbl = s_offline = NULL;
    s_list = NULL;
    s_path[0] = 0;
    s_q[0] = 0;
    s_dl = DL_IDLE;
}

static void on_tick(void)
{
    if (s_offline) {
        ui_offline_update(s_offline);
        if (wifi_ready()) {
            lv_obj_set_hidden(s_offline, true);
        }
    }
    if (s_view != V_PICKER) {
        return;
    }
    if (s_dl == DL_BUSY) {
        size_t done = 0, total = 0;
        os_http_progress(&done, &total);
        if (s_msg && total > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Downloading…\n%u / %u KB",
                     (unsigned)(done / 1024), (unsigned)(total / 1024));
            lv_label_set_text(s_msg, buf);
        }
        return;
    }
    if (s_dl == DL_OK) {
        s_dl = DL_IDLE;
        os_toast("Saved to SD");
        if (s_pending_path[0]) {
            s_restore_y = -1;
            open_book(s_pending_path);
            s_pending_path[0] = 0;
        } else {
            show_picker();
        }
        return;
    }
    if (s_dl == DL_FAIL) {
        s_dl = DL_IDLE;
        os_toast(s_err[0] ? s_err : "Download failed");
        show_picker();
    }
}

static bool on_key(os_key_t key, bool repeat)
{
    if (s_offline && ui_offline_key(s_offline, key)) {
        return true;
    }

    if (s_kb) {
        ui_kb_result_t r = ui_kb_key(s_kb, key, repeat);
        if (r != UI_KB_BUSY) {
            if (r == UI_KB_DONE) {
                strlcpy(s_q, ui_kb_text(s_kb), sizeof(s_q));
            }
            ui_kb_delete(s_kb);
            s_kb = NULL;
            if (r == UI_KB_DONE) {
                clear_picker_ui();
                show_picker();
            }
        }
        return true;
    }

    if (s_view == V_PICKER) {
        if (s_dl == DL_BUSY) {
            return key != OS_KEY_HOME;  // block exit keys except HOME
        }
        if ((key == OS_KEY_LEFT || key == OS_KEY_RIGHT) && !repeat) {
            s_tab = (tab_t)((s_tab + (key == OS_KEY_RIGHT ? 1 : TAB_COUNT - 1)) % TAB_COUNT);
            clear_picker_ui();
            show_picker();
            return true;
        }
        if (s_list && ui_list_key(s_list, key, repeat)) {
            return true;
        }
        if (key == OS_KEY_SELECT && s_list) {
            int i = ui_list_selected(s_list);
            if (s_tab == TAB_CATALOG) {
                if (i == CTRL_SEARCH) {
                    s_kb = ui_kb_create(s_root, "Search catalog", sizeof(s_q) - 1);
                    return true;
                }
                const pg_entry_t *e = catalog_at_list(i);
                if (!e) {
                    return true;
                }
                int catalog_i = s_filt[i - 1];
                char path[PATH_MAX_LEN];
                if (catalog_on_sd(e, path, sizeof(path))) {
                    s_restore_y = -1;
                    char last[PATH_MAX_LEN];
                    int font;
                    int32_t y;
                    load_pos(last, sizeof(last), &font, &y);
                    if (!strcmp(last, path)) {
                        s_font = font;
                        s_restore_y = y;
                    }
                    open_book(path);
                } else {
                    clear_picker_ui();
                    message("Downloading from\nProject Gutenberg…");
                    s_hint = ui_hint(s_root, "");
                    set_picker_hint();
                    if (!start_download(catalog_i)) {
                        show_picker();
                    }
                }
                return true;
            }
            if (i >= 0 && i < s_nbooks) {
                s_restore_y = -1;
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
        return false;
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
        if (s_scroll) {
            lv_obj_scroll_by_bounded(s_scroll, 0, -(VIEW_H - 24), LV_ANIM_ON);
            save_pos();
        }
        return true;
    }
    return false;
}

const os_app_t app_gutenberg = {
    .id = "gutenberg",
    .name = "Gutenberg",
    .symbol = LV_SYMBOL_DIRECTORY,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = exit_app,
    .on_key = on_key,
    .on_tick = on_tick,
};
