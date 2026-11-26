// Dopewars: classic buy-low / sell-high trading sim for the BP26 badge.
// D-pad UI on the 320×240 panel; in-progress run + high score persist in NVS.
// Offline — no Wi-Fi, mesh, or SD required.
//
// Visual theme: shared pocket-battle chrome (sky stage, cream header tile).

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"

#include "core/os.h"
#include "core/theme_battle.h"
#include "core/ui.h"

#define NVS_NS          "dopewars"
#define NVS_SAVE        "save"
#define NVS_HISCORE     "hiscore"
#define SAVE_MAGIC      0x44505731u   /* 'DPW1' */
#define SAVE_VER        1

#define DAYS_MAX        30
#define CASH_START      2000
#define DEBT_START      5500
#define COAT_START      100
#define COAT_STEP       50
#define COAT_PRICE      200
#define DEBT_RATE_PCT   10            /* interest each jet day */
#define LOAN_CITY       0             /* Bronx — loan shark */
#define BANK_CITY       3             /* Manhattan — bank */
#define GOODS_N         6
#define CITIES_N        6
#define EVENT_CHANCE    35            /* % chance of an event when jetting */

#define HDR_H           44
#define LIST_Y          HDR_H
#define LIST_H          (OS_ROOT_H - LIST_Y - UI_HINT_H)

typedef enum {
    V_TITLE = 0,
    V_STREET,
    V_BUY,
    V_SELL,
    V_QTY,
    V_JET,
    V_LOAN,
    V_BANK,
    V_INV,
    V_EVENT,
    V_OVER,
} view_t;

typedef enum {
    ACT_BUY = 0,
    ACT_SELL,
    ACT_JET,
    ACT_LOAN,
    ACT_BANK,
    ACT_INV,
    ACT_QUIT,
    STREET_N,
} street_act_t;

typedef struct {
    const char *name;
    int min_p;
    int max_p;
} good_t;

typedef struct {
    const char *name;
} city_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t day;
    int32_t cash;
    int32_t debt;
    int32_t bank;
    int16_t city;
    int16_t coat;
    int16_t stock[GOODS_N];
    int32_t price[GOODS_N];
    uint8_t avail[GOODS_N];
    uint8_t in_game;        /* 1 = mid-run save is valid */
    uint8_t _pad[3];
} save_t;

static const good_t k_goods[GOODS_N] = {
    { "Acid",       1000,  4400 },
    { "Cocaine",   15000, 30000 },
    { "Hashish",     480,  1280 },
    { "Heroin",     5500, 13000 },
    { "Ludes",        11,    60 },
    { "Weed",        315,  890 },
};

static const city_t k_cities[CITIES_N] = {
    { "Bronx" },
    { "Ghetto" },
    { "Central Park" },
    { "Manhattan" },
    { "Coney Island" },
    { "Brooklyn" },
};

static const char *const k_street[STREET_N] = {
    "Buy",
    "Sell",
    "Jet",
    "Loan shark",
    "Bank",
    "Inventory",
    "Cash out",
};

static const char *const k_title_rows[] = {
    "Continue",
    "New game",
    "High score",
};
#define TITLE_N  3

// ---- state ----------------------------------------------------------------

static lv_obj_t *s_root, *s_hdr, *s_cash, *s_meta, *s_hint, *s_content, *s_body;
static ui_list_t *s_list;
static view_t s_view;
static save_t s_g;
static int32_t s_hiscore;
static bool s_have_save;

static int s_qty_good;          /* index into goods for buy/sell qty */
static bool s_qty_buy;          /* true = buying */
static int s_qty;               /* current amount */
static int s_qty_max;

static char s_event[160];
static char s_over_msg[128];

// ---- RNG ------------------------------------------------------------------

static uint32_t rnd_u32(void)
{
    return esp_random();
}

static int rnd_range(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)(rnd_u32() % (uint32_t)(hi - lo + 1));
}

// ---- NVS ------------------------------------------------------------------

static void hiscore_load(void)
{
    s_hiscore = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, NVS_HISCORE, &v) == ESP_OK && v > 0) {
        s_hiscore = v;
    }
    nvs_close(h);
}

static void hiscore_save(int32_t score)
{
    if (score <= s_hiscore) {
        return;
    }
    s_hiscore = score;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, NVS_HISCORE, s_hiscore);
    nvs_commit(h);
    nvs_close(h);
}

static bool save_load(save_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, NVS_SAVE, out, &n);
    nvs_close(h);
    if (err != ESP_OK || n != sizeof(*out)) {
        return false;
    }
    if (out->magic != SAVE_MAGIC || out->ver != SAVE_VER || !out->in_game) {
        return false;
    }
    if (out->city < 0 || out->city >= CITIES_N || out->day < 1 || out->day > DAYS_MAX + 1) {
        return false;
    }
    return true;
}

static void save_store(void)
{
    s_g.magic = SAVE_MAGIC;
    s_g.ver = SAVE_VER;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NVS_SAVE, &s_g, sizeof(s_g));
    nvs_commit(h);
    nvs_close(h);
    s_have_save = s_g.in_game != 0;
}

static void save_clear(void)
{
    s_g.in_game = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, NVS_SAVE);
    nvs_commit(h);
    nvs_close(h);
    s_have_save = false;
}

// ---- economy --------------------------------------------------------------

static int coat_used(void)
{
    int n = 0;
    for (int i = 0; i < GOODS_N; i++) {
        n += s_g.stock[i];
    }
    return n;
}

static int32_t net_worth(void)
{
    int32_t w = s_g.cash + s_g.bank - s_g.debt;
    for (int i = 0; i < GOODS_N; i++) {
        if (s_g.stock[i] > 0 && s_g.avail[i]) {
            w += (int32_t)s_g.stock[i] * s_g.price[i];
        } else if (s_g.stock[i] > 0) {
            /* mid price estimate when not offered here */
            w += (int32_t)s_g.stock[i] * ((k_goods[i].min_p + k_goods[i].max_p) / 2);
        }
    }
    return w;
}

static void roll_prices(void)
{
    for (int i = 0; i < GOODS_N; i++) {
        /* ~85% chance a good is offered in this city */
        s_g.avail[i] = rnd_range(1, 100) <= 85 ? 1 : 0;
        int p = rnd_range(k_goods[i].min_p, k_goods[i].max_p);
        /* rare spikes / dumps */
        int roll = rnd_range(1, 100);
        if (roll <= 4) {
            p = p * 2 + k_goods[i].max_p / 2;
            if (p > k_goods[i].max_p * 3) {
                p = k_goods[i].max_p * 3;
            }
        } else if (roll <= 8) {
            p = p / 2;
            if (p < k_goods[i].min_p / 2) {
                p = k_goods[i].min_p / 2;
            }
            if (p < 1) {
                p = 1;
            }
        }
        s_g.price[i] = p;
    }
}

static void game_new(void)
{
    memset(&s_g, 0, sizeof(s_g));
    s_g.magic = SAVE_MAGIC;
    s_g.ver = SAVE_VER;
    s_g.day = 1;
    s_g.cash = CASH_START;
    s_g.debt = DEBT_START;
    s_g.bank = 0;
    s_g.city = LOAN_CITY;
    s_g.coat = COAT_START;
    s_g.in_game = 1;
    roll_prices();
    save_store();
}

static void debt_interest(void)
{
    if (s_g.debt <= 0) {
        return;
    }
    int64_t add = ((int64_t)s_g.debt * DEBT_RATE_PCT) / 100;
    if (add < 1) {
        add = 1;
    }
    s_g.debt += (int32_t)add;
    if (s_g.debt > 99999999) {
        s_g.debt = 99999999;
    }
}

static bool finish_if_done(void)
{
    if (s_g.day <= DAYS_MAX) {
        return false;
    }
    int32_t score = s_g.cash + s_g.bank - s_g.debt;
    for (int i = 0; i < GOODS_N; i++) {
        /* leftover inventory scores at half mid */
        if (s_g.stock[i] > 0) {
            score += (int32_t)s_g.stock[i] * ((k_goods[i].min_p + k_goods[i].max_p) / 4);
        }
    }
    bool beat = score > s_hiscore;
    hiscore_save(score);
    if (score >= 0) {
        snprintf(s_over_msg, sizeof(s_over_msg),
                 "Day %d done.\nNet $%ld%s",
                 DAYS_MAX, (long)score, beat ? "\nNEW HIGH SCORE!" : "");
    } else {
        snprintf(s_over_msg, sizeof(s_over_msg),
                 "Busted.\nNet $%ld\n(in the red)", (long)score);
    }
    s_g.in_game = 0;
    save_clear();
    return true;
}

static void make_event(void)
{
    int roll = rnd_range(1, 100);
    s_event[0] = 0;

    if (roll <= 12) {
        /* cops — dump some random stock */
        int dumped = 0;
        for (int i = 0; i < GOODS_N; i++) {
            if (s_g.stock[i] <= 0) {
                continue;
            }
            int lose = s_g.stock[i] / 2 + rnd_range(0, 2);
            if (lose > s_g.stock[i]) {
                lose = s_g.stock[i];
            }
            if (lose > 0) {
                s_g.stock[i] -= (int16_t)lose;
                dumped += lose;
            }
        }
        if (dumped > 0) {
            snprintf(s_event, sizeof(s_event),
                     "Cops! You ditch %d units running through the alleys.", dumped);
        } else {
            snprintf(s_event, sizeof(s_event),
                     "Cops cruise by. Your pockets are empty — lucky.");
        }
    } else if (roll <= 22) {
        int fine = rnd_range(200, 800);
        if (fine > s_g.cash) {
            fine = s_g.cash;
        }
        s_g.cash -= fine;
        snprintf(s_event, sizeof(s_event),
                 "Mugged! Lost $%d. Watch your back.", fine);
    } else if (roll <= 32) {
        int find = rnd_range(100, 600);
        s_g.cash += find;
        snprintf(s_event, sizeof(s_event),
                 "Found a wallet on the train. +$%d.", find);
    } else if (roll <= 42) {
        int g = rnd_range(0, GOODS_N - 1);
        s_g.avail[g] = 1;
        s_g.price[g] = k_goods[g].min_p / 2;
        if (s_g.price[g] < 1) {
            s_g.price[g] = 1;
        }
        snprintf(s_event, sizeof(s_event),
                 "Street tip: %s is dirt cheap here!", k_goods[g].name);
    } else if (roll <= 52) {
        int g = rnd_range(0, GOODS_N - 1);
        s_g.avail[g] = 1;
        s_g.price[g] = k_goods[g].max_p * 2;
        snprintf(s_event, sizeof(s_event),
                 "Shortage! %s prices went crazy.", k_goods[g].name);
    } else if (roll <= 62) {
        int space = s_g.coat - coat_used();
        if (space > 0) {
            int g = rnd_range(0, GOODS_N - 1);
            int n = rnd_range(1, space > 10 ? 10 : space);
            s_g.stock[g] += (int16_t)n;
            snprintf(s_event, sizeof(s_event),
                     "A stranger slips you %d %s. No questions.", n, k_goods[g].name);
        } else {
            snprintf(s_event, sizeof(s_event),
                     "A stranger offers a gift, but your coat is full.");
        }
    } else if (roll <= 72) {
        if (s_g.cash >= COAT_PRICE) {
            /* COAT: prefix → SELECT buys in event view */
            snprintf(s_event, sizeof(s_event),
                     "COAT:%d Trench coat: +%d capacity for $%d.",
                     COAT_PRICE, COAT_STEP, COAT_PRICE);
        } else {
            snprintf(s_event, sizeof(s_event),
                     "You window-shop a bigger coat. Need $%d.", COAT_PRICE);
        }
    } else {
        snprintf(s_event, sizeof(s_event),
                 "Uneventful ride. The city hums on.");
    }
}

static void jet_to(int city)
{
    if (city < 0 || city >= CITIES_N || city == s_g.city) {
        return;
    }
    s_g.city = (int16_t)city;
    s_g.day++;
    debt_interest();
    roll_prices();
    if (rnd_range(1, 100) <= EVENT_CHANCE) {
        make_event();
    } else {
        s_event[0] = 0;
    }
    save_store();
}

// ---- UI helpers -----------------------------------------------------------

static void set_hint(const char *text)
{
    if (s_hint) {
        lv_label_set_text(s_hint, text);
    }
}

static void clear_content(void)
{
    if (s_content) {
        lv_obj_delete(s_content);
        s_content = NULL;
    }
    s_list = NULL;
    s_body = NULL;
}

static void hdr_refresh(void)
{
    if (!s_cash || !s_meta) {
        return;
    }
    char line[64];
    snprintf(line, sizeof(line), "$%ld", (long)s_g.cash);
    lv_label_set_text(s_cash, line);

    if (s_view == V_TITLE || s_view == V_OVER) {
        lv_label_set_text(s_meta, "Dopewars");
        return;
    }
    snprintf(line, sizeof(line), "Day %d/%d  %s",
             (int)s_g.day, DAYS_MAX, k_cities[s_g.city].name);
    lv_label_set_text(s_meta, line);

    char debt[48];
    if (s_g.debt > 0) {
        snprintf(debt, sizeof(debt), "debt $%ld", (long)s_g.debt);
    } else if (s_g.bank > 0) {
        snprintf(debt, sizeof(debt), "bank $%ld", (long)s_g.bank);
    } else {
        snprintf(debt, sizeof(debt), "coat %d/%d", coat_used(), (int)s_g.coat);
    }
    /* reuse hdr subtitle area via s_hdr child — keep meta to day/city;
       debt shown in cash line right side via strcat-style second label? */
    (void)debt;
}

static void hdr_debt_line(void)
{
    /* second line under cash already used by meta; append debt into cash label */
    if (!s_cash || s_view == V_TITLE || s_view == V_OVER) {
        return;
    }
    char line[72];
    snprintf(line, sizeof(line), "$%ld  debt $%ld", (long)s_g.cash, (long)s_g.debt);
    lv_label_set_text(s_cash, line);
}

static lv_obj_t *make_panel(void)
{
    clear_content();
    s_content = battle_card(s_root, 8, LIST_Y, OS_ROOT_W - 16, LIST_H - 4, T_YELLOW);
    s_body = battle_label(s_content, &sol_font_14, T_INK, "");
    lv_obj_set_width(s_body, OS_ROOT_W - 32);
    lv_obj_set_style_pad_all(s_body, 8, 0);
    lv_obj_set_pos(s_body, 8, BATTLE_STRIPE_H + 4);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_MODE_WRAP);
    return s_content;
}

static void show_list(ui_row_cb_t cb, int count, int select)
{
    clear_content();
    s_content = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 0, LIST_Y);
    lv_obj_set_size(s_content, OS_ROOT_W, LIST_H);
    lv_obj_set_scrollable(s_content, false);
    s_list = ui_list_create(s_content, 0, LIST_H, cb, NULL);
    ui_list_set_count(s_list, count);
    if (count > 0) {
        ui_list_select(s_list, select < 0 ? 0 : (select >= count ? count - 1 : select));
    }
}

// ---- row callbacks --------------------------------------------------------

static void title_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    strlcpy(row->title, k_title_rows[i], sizeof(row->title));
    row->sub[0] = 0;
    if (i == 0) {
        if (s_have_save) {
            snprintf(row->sub, sizeof(row->sub), "Day %d · %s",
                     (int)s_g.day, k_cities[s_g.city].name);
            strlcpy(row->right, "NVS", sizeof(row->right));
            row->right_color = COL_GREEN;
        } else {
            strlcpy(row->sub, "No saved run", sizeof(row->sub));
            row->title_color = COL_DIM;
        }
    } else if (i == 2) {
        snprintf(row->right, sizeof(row->right), "$%ld", (long)s_hiscore);
        row->right_color = COL_YELLOW;
    }
}

static void street_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    strlcpy(row->title, k_street[i], sizeof(row->title));
    row->sub[0] = 0;
    if (i == ACT_LOAN && s_g.city != LOAN_CITY) {
        strlcpy(row->sub, "Only in the Bronx", sizeof(row->sub));
        row->title_color = COL_DIM;
    } else if (i == ACT_BANK && s_g.city != BANK_CITY) {
        strlcpy(row->sub, "Only in Manhattan", sizeof(row->sub));
        row->title_color = COL_DIM;
    } else if (i == ACT_BUY) {
        int n = 0;
        for (int g = 0; g < GOODS_N; g++) {
            n += s_g.avail[g];
        }
        snprintf(row->sub, sizeof(row->sub), "%d goods on offer", n);
    } else if (i == ACT_SELL) {
        snprintf(row->sub, sizeof(row->sub), "%d/%d coat", coat_used(), (int)s_g.coat);
    } else if (i == ACT_JET) {
        snprintf(row->right, sizeof(row->right), "day %d", (int)s_g.day);
    }
}

static void buy_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    /* i indexes available goods compactly — build map each time */
    int seen = -1;
    int g = -1;
    for (int k = 0; k < GOODS_N; k++) {
        if (!s_g.avail[k]) {
            continue;
        }
        seen++;
        if (seen == i) {
            g = k;
            break;
        }
    }
    if (g < 0) {
        strlcpy(row->title, "?", sizeof(row->title));
        return;
    }
    strlcpy(row->title, k_goods[g].name, sizeof(row->title));
    snprintf(row->sub, sizeof(row->sub), "have %d", (int)s_g.stock[g]);
    snprintf(row->right, sizeof(row->right), "$%ld", (long)s_g.price[g]);
    if (s_g.price[g] <= k_goods[g].min_p + (k_goods[g].max_p - k_goods[g].min_p) / 4) {
        row->right_color = COL_GREEN;
    } else if (s_g.price[g] >= k_goods[g].max_p - (k_goods[g].max_p - k_goods[g].min_p) / 4) {
        row->right_color = COL_ORANGE;
    }
}

static void sell_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    int seen = -1;
    int g = -1;
    for (int k = 0; k < GOODS_N; k++) {
        if (s_g.stock[k] <= 0) {
            continue;
        }
        seen++;
        if (seen == i) {
            g = k;
            break;
        }
    }
    if (g < 0) {
        strlcpy(row->title, "?", sizeof(row->title));
        return;
    }
    strlcpy(row->title, k_goods[g].name, sizeof(row->title));
    snprintf(row->sub, sizeof(row->sub), "x%d", (int)s_g.stock[g]);
    if (s_g.avail[g]) {
        snprintf(row->right, sizeof(row->right), "$%ld", (long)s_g.price[g]);
        row->right_color = COL_GREEN;
    } else {
        strlcpy(row->right, "no bid", sizeof(row->right));
        row->right_color = COL_DIM;
        row->title_color = COL_DIM;
    }
}

static void jet_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    strlcpy(row->title, k_cities[i].name, sizeof(row->title));
    if (i == s_g.city) {
        strlcpy(row->sub, "You are here", sizeof(row->sub));
        strlcpy(row->right, "now", sizeof(row->right));
        row->title_color = COL_AQUA;
        row->right_color = COL_AQUA;
    } else {
        if (i == LOAN_CITY) {
            strlcpy(row->sub, "Loan shark", sizeof(row->sub));
        } else if (i == BANK_CITY) {
            strlcpy(row->sub, "Bank", sizeof(row->sub));
        }
        strlcpy(row->right, "+1 day", sizeof(row->right));
    }
}

static void loan_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    if (i == 0) {
        strlcpy(row->title, "Pay debt", sizeof(row->title));
        snprintf(row->sub, sizeof(row->sub), "Owe $%ld", (long)s_g.debt);
        strlcpy(row->right, "SELECT", sizeof(row->right));
    } else {
        strlcpy(row->title, "Borrow more", sizeof(row->title));
        strlcpy(row->sub, "+$1000 · interest bites", sizeof(row->sub));
        strlcpy(row->right, "+1k", sizeof(row->right));
    }
}

static void bank_row(int i, ui_row_t *row, void *ctx)
{
    (void)ctx;
    if (i == 0) {
        strlcpy(row->title, "Deposit $500", sizeof(row->title));
        snprintf(row->sub, sizeof(row->sub), "Pocket $%ld", (long)s_g.cash);
    } else {
        strlcpy(row->title, "Withdraw $500", sizeof(row->title));
        snprintf(row->sub, sizeof(row->sub), "Bank $%ld", (long)s_g.bank);
    }
}

// ---- forward --------------------------------------------------------------

static void show_title(void);
static void show_street(void);
static void show_buy(void);
static void show_sell(void);
static void show_qty(void);
static void show_jet(void);
static void show_loan(void);
static void show_bank(void);
static void show_inv(void);
static void show_event(void);
static void show_over(void);

static int count_avail(void)
{
    int n = 0;
    for (int i = 0; i < GOODS_N; i++) {
        n += s_g.avail[i] ? 1 : 0;
    }
    return n;
}

static int count_stock(void)
{
    int n = 0;
    for (int i = 0; i < GOODS_N; i++) {
        if (s_g.stock[i] > 0) {
            n++;
        }
    }
    return n;
}

static int avail_at(int index)
{
    int seen = -1;
    for (int k = 0; k < GOODS_N; k++) {
        if (!s_g.avail[k]) {
            continue;
        }
        seen++;
        if (seen == index) {
            return k;
        }
    }
    return -1;
}

static int stock_at(int index)
{
    int seen = -1;
    for (int k = 0; k < GOODS_N; k++) {
        if (s_g.stock[k] <= 0) {
            continue;
        }
        seen++;
        if (seen == index) {
            return k;
        }
    }
    return -1;
}

static void refresh_hdr(void)
{
    hdr_refresh();
    hdr_debt_line();
}

static void show_title(void)
{
    s_view = V_TITLE;
    /* peek save for Continue row */
    save_t tmp;
    s_have_save = save_load(&tmp);
    if (s_have_save) {
        s_g = tmp;
    }
    show_list(title_row, TITLE_N, s_have_save ? 0 : 1);
    if (s_cash) {
        lv_label_set_text(s_cash, "Dopewars");
    }
    if (s_meta) {
        char line[48];
        snprintf(line, sizeof(line), "High $%ld", (long)s_hiscore);
        lv_label_set_text(s_meta, line);
    }
    set_hint("SELECT choose  CANCEL exit");
}

static void show_street(void)
{
    if (finish_if_done()) {
        show_over();
        return;
    }
    s_view = V_STREET;
    show_list(street_row, STREET_N, 0);
    refresh_hdr();
    set_hint("SELECT  CANCEL title");
}

static void show_buy(void)
{
    s_view = V_BUY;
    int n = count_avail();
    show_list(buy_row, n, 0);
    if (n == 0) {
        ui_list_set_empty(s_list, "Nothing for sale here");
    }
    refresh_hdr();
    set_hint("SELECT buy  CANCEL street");
}

static void show_sell(void)
{
    s_view = V_SELL;
    int n = count_stock();
    show_list(sell_row, n, 0);
    if (n == 0) {
        ui_list_set_empty(s_list, "Coat is empty");
    }
    refresh_hdr();
    set_hint("SELECT sell  CANCEL street");
}

static void qty_paint(void)
{
    make_panel();
    const char *name = k_goods[s_qty_good].name;
    int32_t price = s_g.price[s_qty_good];
    char buf[192];
    snprintf(buf, sizeof(buf),
             "%s %s\n\n"
             "$%ld each\n"
             "Amount: %d / %d\n\n"
             "Total $%ld\n"
             "Cash   $%ld",
             s_qty_buy ? "Buy" : "Sell", name,
             (long)price, s_qty, s_qty_max,
             (long)price * s_qty, (long)s_g.cash);
    lv_label_set_text(s_body, buf);
    set_hint(LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " qty  SELECT ok  CANCEL back");
    refresh_hdr();
}

static void show_qty(void)
{
    s_view = V_QTY;
    qty_paint();
}

static void show_jet(void)
{
    s_view = V_JET;
    show_list(jet_row, CITIES_N, s_g.city);
    refresh_hdr();
    set_hint("SELECT fly (+1 day)  CANCEL");
}

static void show_loan(void)
{
    s_view = V_LOAN;
    show_list(loan_row, 2, 0);
    refresh_hdr();
    set_hint("SELECT  CANCEL street");
}

static void show_bank(void)
{
    s_view = V_BANK;
    show_list(bank_row, 2, 0);
    refresh_hdr();
    set_hint("SELECT  CANCEL street");
}

static void show_inv(void)
{
    s_view = V_INV;
    make_panel();
    char buf[320];
    size_t n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "Net ~$%ld\nBank $%ld  Coat %d/%d\n\n",
                  (long)net_worth(), (long)s_g.bank, coat_used(), (int)s_g.coat);
    for (int i = 0; i < GOODS_N && n + 40 < sizeof(buf); i++) {
        if (s_g.stock[i] <= 0) {
            continue;
        }
        n += snprintf(buf + n, sizeof(buf) - n, "%s x%d\n",
                      k_goods[i].name, (int)s_g.stock[i]);
    }
    if (coat_used() == 0) {
        n += snprintf(buf + n, sizeof(buf) - n, "(empty coat)\n");
    }
    lv_label_set_text(s_body, buf);
    set_hint("CANCEL street");
    refresh_hdr();
}

static void show_event(void)
{
    s_view = V_EVENT;
    make_panel();
    const char *msg = s_event;
    if (!strncmp(s_event, "COAT:", 5)) {
        msg = s_event + 5;
        while (*msg && *msg != ' ') {
            msg++;
        }
        if (*msg == ' ') {
            msg++;
        }
        set_hint("SELECT buy coat  CANCEL skip");
    } else {
        set_hint("SELECT continue");
    }
    lv_label_set_text(s_body, msg[0] ? msg : "Something happened.");
    refresh_hdr();
}

static void show_over(void)
{
    s_view = V_OVER;
    make_panel();
    lv_label_set_text(s_body, s_over_msg[0] ? s_over_msg : "Game over.");
    if (s_cash) {
        lv_label_set_text(s_cash, "Game over");
    }
    if (s_meta) {
        char line[48];
        snprintf(line, sizeof(line), "High $%ld", (long)s_hiscore);
        lv_label_set_text(s_meta, line);
    }
    set_hint("SELECT title  CANCEL exit");
}

// ---- actions --------------------------------------------------------------

static void begin_buy(int good)
{
    if (good < 0 || !s_g.avail[good]) {
        return;
    }
    int space = s_g.coat - coat_used();
    if (space <= 0) {
        os_toast("Coat full");
        return;
    }
    int afford = s_g.price[good] > 0 ? (int)(s_g.cash / s_g.price[good]) : 0;
    s_qty_max = afford < space ? afford : space;
    if (s_qty_max <= 0) {
        os_toast("Can't afford");
        return;
    }
    s_qty_good = good;
    s_qty_buy = true;
    s_qty = 1;
    show_qty();
}

static void begin_sell(int good)
{
    if (good < 0 || s_g.stock[good] <= 0) {
        return;
    }
    if (!s_g.avail[good]) {
        os_toast("No buyers here");
        return;
    }
    s_qty_good = good;
    s_qty_buy = false;
    s_qty_max = s_g.stock[good];
    s_qty = s_qty_max;
    show_qty();
}

static void commit_qty(void)
{
    if (s_qty < 1 || s_qty > s_qty_max) {
        return;
    }
    int32_t price = s_g.price[s_qty_good];
    int64_t total = (int64_t)price * s_qty;
    if (s_qty_buy) {
        if (total > s_g.cash) {
            os_toast("Not enough cash");
            return;
        }
        s_g.cash -= (int32_t)total;
        s_g.stock[s_qty_good] += (int16_t)s_qty;
        os_toast("Bought %d", s_qty);
        save_store();
        show_buy();
    } else {
        s_g.cash += (int32_t)total;
        s_g.stock[s_qty_good] -= (int16_t)s_qty;
        os_toast("Sold %d", s_qty);
        save_store();
        show_sell();
    }
}

static void cash_out_done(bool yes, void *ctx)
{
    (void)ctx;
    if (!yes) {
        return;
    }
    s_g.day = DAYS_MAX + 1;
    if (finish_if_done()) {
        show_over();
    }
}

static void title_select(int i)
{
    if (i == 0) {
        if (!s_have_save || !save_load(&s_g)) {
            os_toast("No save");
            return;
        }
        show_street();
        return;
    }
    if (i == 1) {
        game_new();
        show_street();
        return;
    }
    /* high score toast */
    os_toast("High score $%ld", (long)s_hiscore);
}

static void street_select(int i)
{
    switch (i) {
    case ACT_BUY:
        show_buy();
        break;
    case ACT_SELL:
        show_sell();
        break;
    case ACT_JET:
        show_jet();
        break;
    case ACT_LOAN:
        if (s_g.city != LOAN_CITY) {
            os_toast("Shark is in the Bronx");
            break;
        }
        show_loan();
        break;
    case ACT_BANK:
        if (s_g.city != BANK_CITY) {
            os_toast("Bank is in Manhattan");
            break;
        }
        show_bank();
        break;
    case ACT_INV:
        show_inv();
        break;
    case ACT_QUIT:
        os_confirm("Cash out now?\nScore pocket+bank-debt.", cash_out_done, NULL);
        break;
    default:
        break;
    }
}

static void loan_select(int i)
{
    if (i == 0) {
        /* pay as much as possible / all */
        if (s_g.debt <= 0) {
            os_toast("Debt clear");
            return;
        }
        int32_t pay = s_g.cash < s_g.debt ? s_g.cash : s_g.debt;
        if (pay <= 0) {
            os_toast("No cash");
            return;
        }
        s_g.cash -= pay;
        s_g.debt -= pay;
        os_toast("Paid $%ld", (long)pay);
        save_store();
        show_loan();
        return;
    }
    s_g.debt += 1000;
    s_g.cash += 1000;
    os_toast("Borrowed $1000");
    save_store();
    show_loan();
}

static void bank_select(int i)
{
    if (i == 0) {
        if (s_g.cash < 500) {
            os_toast("Need $500");
            return;
        }
        s_g.cash -= 500;
        s_g.bank += 500;
        os_toast("Deposited $500");
    } else {
        if (s_g.bank < 500) {
            os_toast("Bank low");
            return;
        }
        s_g.bank -= 500;
        s_g.cash += 500;
        os_toast("Withdrew $500");
    }
    save_store();
    show_bank();
}

static void jet_select(int i)
{
    if (i == s_g.city) {
        os_toast("Already here");
        return;
    }
    jet_to(i);
    if (finish_if_done()) {
        show_over();
        return;
    }
    if (s_event[0]) {
        show_event();
    } else {
        show_street();
    }
}

static void event_select(void)
{
    if (!strncmp(s_event, "COAT:", 5)) {
        int price = COAT_PRICE;
        if (s_g.cash >= price) {
            s_g.cash -= price;
            s_g.coat += COAT_STEP;
            os_toast("Coat +%d", COAT_STEP);
            save_store();
        } else {
            os_toast("Too poor");
        }
    }
    s_event[0] = 0;
    show_street();
}

// ---- lifecycle ------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_list = NULL;
    s_content = NULL;
    s_body = NULL;
    s_event[0] = 0;
    s_over_msg[0] = 0;

    hiscore_load();
    battle_paint_stage(root);

    s_hdr = battle_card(root, 4, 2, OS_ROOT_W - 8, HDR_H, T_ORANGE);
    s_cash = battle_label(s_hdr, &sol_font_20, T_NAVY, "Dopewars");
    lv_obj_set_pos(s_cash, 10, BATTLE_STRIPE_H + 2);
    lv_obj_set_width(s_cash, OS_ROOT_W - 28);
    lv_label_set_long_mode(s_cash, LV_LABEL_LONG_MODE_DOTS);

    s_meta = battle_label(s_hdr, &sol_mono_12, T_DIM, "");
    lv_obj_set_pos(s_meta, 10, BATTLE_STRIPE_H + 22);
    lv_obj_set_width(s_meta, OS_ROOT_W - 28);
    lv_label_set_long_mode(s_meta, LV_LABEL_LONG_MODE_DOTS);

    s_hint = battle_hint(root, "");
    show_title();
}

static void leave(void)
{
    if (s_g.in_game) {
        save_store();
    }
    s_root = s_hdr = s_cash = s_meta = s_hint = s_content = s_body = NULL;
    s_list = NULL;
}

static bool key(os_key_t k, bool repeat)
{
    if (s_view == V_QTY) {
        if (k == OS_KEY_LEFT) {
            int step = repeat ? 5 : 1;
            s_qty -= step;
            if (s_qty < 1) {
                s_qty = 1;
            }
            qty_paint();
            return true;
        }
        if (k == OS_KEY_RIGHT) {
            int step = repeat ? 5 : 1;
            s_qty += step;
            if (s_qty > s_qty_max) {
                s_qty = s_qty_max;
            }
            qty_paint();
            return true;
        }
        if (k == OS_KEY_UP && !repeat) {
            s_qty = s_qty_max;
            qty_paint();
            return true;
        }
        if (k == OS_KEY_DOWN && !repeat) {
            s_qty = 1;
            qty_paint();
            return true;
        }
        if (k == OS_KEY_SELECT && !repeat) {
            commit_qty();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            if (s_qty_buy) {
                show_buy();
            } else {
                show_sell();
            }
            return true;
        }
        return false;
    }

    if (s_view == V_EVENT) {
        if (k == OS_KEY_SELECT && !repeat) {
            event_select();
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            if (!strncmp(s_event, "COAT:", 5)) {
                s_event[0] = 0;
                show_street();
                return true;
            }
            event_select();
            return true;
        }
        return true;
    }

    if (s_view == V_INV) {
        if (k == OS_KEY_BACK && !repeat) {
            show_street();
            return true;
        }
        return k != OS_KEY_BACK;
    }

    if (s_view == V_OVER) {
        if (k == OS_KEY_SELECT && !repeat) {
            show_title();
            return true;
        }
        return false; /* CANCEL exits app */
    }

    if (s_list && ui_list_key(s_list, k, repeat)) {
        return true;
    }

    if (k == OS_KEY_SELECT && !repeat) {
        int sel = s_list ? ui_list_selected(s_list) : -1;
        switch (s_view) {
        case V_TITLE:
            if (sel >= 0) {
                title_select(sel);
            }
            return true;
        case V_STREET:
            if (sel >= 0) {
                street_select(sel);
            }
            return true;
        case V_BUY:
            if (sel >= 0) {
                begin_buy(avail_at(sel));
            }
            return true;
        case V_SELL:
            if (sel >= 0) {
                begin_sell(stock_at(sel));
            }
            return true;
        case V_JET:
            if (sel >= 0) {
                jet_select(sel);
            }
            return true;
        case V_LOAN:
            if (sel >= 0) {
                loan_select(sel);
            }
            return true;
        case V_BANK:
            if (sel >= 0) {
                bank_select(sel);
            }
            return true;
        default:
            break;
        }
        return true;
    }

    if (k == OS_KEY_BACK && !repeat) {
        switch (s_view) {
        case V_TITLE:
            return false; /* exit app */
        case V_STREET:
            save_store();
            show_title();
            return true;
        case V_BUY:
        case V_SELL:
        case V_JET:
        case V_LOAN:
        case V_BANK:
            show_street();
            return true;
        default:
            break;
        }
    }
    return false;
}

const os_app_t app_dopewars = {
    .id = "dopewars",
    .name = "Dopewars",
    .symbol = LV_SYMBOL_CHARGE,
    .accent = T_ORANGE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
};
