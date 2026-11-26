// Open-Meteo weather for BP26.
//
// Fetches https://api.open-meteo.com/v1/forecast over Wi-Fi with os_http
// (no API key): current conditions + a short hourly strip. City search uses
// the Open-Meteo Geocoding API. Lat/lon also from NVS / SD. Pocket-battle
// palette on the 320×240 panel.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bsp.h"
#include "core/os.h"
#include "core/os_cmd.h"
#include "core/os_http.h"
#include "core/os_wifi.h"
#include "core/theme_battle.h"
#include "core/ui.h"

static const char *TAG = "weather";

#define NVS_NS          "weather"
#define LAT_FILE        BSP_SD_MOUNT "/weather.lat"
#define LON_FILE        BSP_SD_MOUNT "/weather.lon"
#define LOC_FILE        BSP_SD_MOUNT "/weather.loc"

#define COORD_MAX       32
#define CITY_MAX        48
#define DETAIL_MAX      56
#define URL_MAX         400
#define RESP_CAP        8192
#define WORKER_STACK    8192
#define CONTENT_H       (OS_ROOT_H - UI_HINT_H)
#define HOURLY_N        6
#define GEO_MAX         5
#define CITY_QUERY_MAX  40

/* Default: London (documented). Override via NVS / SD / city search. */
#define DEFAULT_LAT     "51.5074"
#define DEFAULT_LON     "-0.1278"
#define DEFAULT_CITY    "London"

/* Palette from theme_battle.h; rain accent + stripe alias local. */
#define T_RAIN          0x1B6CA8
#define STRIPE_H        BATTLE_STRIPE_H

typedef enum {
    FETCH_IDLE = 0,
    FETCH_BUSY,
    FETCH_OK,
    FETCH_FAIL,
} fetch_t;

typedef enum {
    JOB_NONE = 0,
    JOB_FORECAST,
    JOB_GEOCODE,
} job_t;

typedef enum {
    UI_VIEW = 0,
    UI_CITY_PICK,
} ui_mode_t;

typedef struct {
    char hour[8];
    float temp_c;
    float precip_mm;
    int weather_code;
} hour_t;

typedef struct {
    float temp_c;
    float feels_c;
    float humidity;
    float wind_kmh;
    float wind_dir;
    float precip_mm;
    int weather_code;
    char time[40];
    hour_t hourly[HOURLY_N];
    int hourly_n;
    bool valid;
} wx_t;

typedef struct {
    char name[CITY_MAX];
    char detail[DETAIL_MAX];
    char lat[COORD_MAX];
    char lon[COORD_MAX];
} geo_hit_t;

static lv_obj_t *s_root, *s_offline, *s_hint;
static lv_obj_t *s_card, *s_loc, *s_temp, *s_cond, *s_meta, *s_status;
static lv_obj_t *s_icon;
static lv_obj_t *s_hour_row;
static lv_obj_t *s_hour_lbl[HOURLY_N];
static lv_obj_t *s_pick_lbl[GEO_MAX];

static char s_lat[COORD_MAX];
static char s_lon[COORD_MAX];
static char s_city[CITY_MAX];
static bool s_fahrenheit;
static bool s_using_default;

static wx_t s_wx;
static volatile fetch_t s_fetch;
static volatile job_t s_job;
static char s_err[96];
static wx_t s_pending;
static char s_pending_err[96];

static geo_hit_t s_geo[GEO_MAX];
static geo_hit_t s_pending_geo[GEO_MAX];
static int s_geo_n, s_pending_geo_n;
static int s_geo_sel;
static char s_city_query[CITY_QUERY_MAX];
static ui_mode_t s_mode;
static ui_kb_t *s_kb;

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

static bool nvs_get(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = 0;
        return false;
    }
    trim_inplace(out);
    return out[0] != 0;
}

static bool nvs_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, key, val ? val : "");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static bool parse_coord(const char *s, float *out)
{
    if (!s || !s[0]) {
        return false;
    }
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s) {
        return false;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end) {
        return false;
    }
    *out = v;
    return true;
}

static bool coords_ok(void)
{
    float lat = 0, lon = 0;
    if (!parse_coord(s_lat, &lat) || !parse_coord(s_lon, &lon)) {
        return false;
    }
    return lat >= -90.0f && lat <= 90.0f && lon >= -180.0f && lon <= 180.0f;
}

static void load_config(void)
{
    s_lat[0] = s_lon[0] = s_city[0] = 0;
    s_using_default = false;
    bool have_lat = nvs_get("lat", s_lat, sizeof(s_lat));
    bool have_lon = nvs_get("lon", s_lon, sizeof(s_lon));
    nvs_get("city", s_city, sizeof(s_city));
    if (!have_lat) {
        have_lat = read_file_line(LAT_FILE, s_lat, sizeof(s_lat));
    }
    if (!have_lon) {
        have_lon = read_file_line(LON_FILE, s_lon, sizeof(s_lon));
    }
    if ((!have_lat || !have_lon)) {
        char buf[80];
        if (read_file_line(LOC_FILE, buf, sizeof(buf))) {
            char *sep = strchr(buf, ',');
            if (!sep) {
                sep = strchr(buf, ' ');
            }
            if (sep) {
                *sep++ = 0;
                trim_inplace(buf);
                trim_inplace(sep);
                if (!have_lat && buf[0]) {
                    strlcpy(s_lat, buf, sizeof(s_lat));
                    have_lat = true;
                }
                if (!have_lon && sep[0]) {
                    strlcpy(s_lon, sep, sizeof(s_lon));
                    have_lon = true;
                }
            }
        }
    }
    {
        char units[8];
        if (nvs_get("units", units, sizeof(units))) {
            s_fahrenheit = (units[0] == 'f' || units[0] == 'F');
        }
    }
    if (!have_lat || !have_lon || !coords_ok()) {
        strlcpy(s_lat, DEFAULT_LAT, sizeof(s_lat));
        strlcpy(s_lon, DEFAULT_LON, sizeof(s_lon));
        if (!s_city[0]) {
            strlcpy(s_city, DEFAULT_CITY, sizeof(s_city));
        }
        s_using_default = true;
    }
}

static bool wifi_ready(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED;
}

static const char *wmo_text(int code)
{
    switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Rime fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Dense drizzle";
    case 56: return "Freezing drizzle";
    case 57: return "Dense frz drizzle";
    case 61: return "Slight rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: return "Freezing rain";
    case 67: return "Heavy frz rain";
    case 71: return "Slight snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Rain showers";
    case 81: return "Mod. showers";
    case 82: return "Violent showers";
    case 85: return "Snow showers";
    case 86: return "Heavy snow shwr";
    case 95: return "Thunderstorm";
    case 96: return "Storm + hail";
    case 97: return "Heavy storm";
    case 99: return "Storm + heavy hail";
    default: return "Unknown";
    }
}

/* Compact rain / condition icons (ASCII — sol fonts have no emoji). */
static const char *wmo_icon(int code)
{
    if (code == 0 || code == 1) {
        return "*";     /* sun / clear */
    }
    if (code == 2 || code == 3) {
        return "~";     /* cloud */
    }
    if (code == 45 || code == 48) {
        return "=";     /* fog */
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        return "/";     /* rain / drizzle / showers */
    }
    if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
        return "#";     /* snow */
    }
    if (code >= 95) {
        return "!";     /* thunder */
    }
    return "?";
}

static bool wmo_is_rain(int code)
{
    return (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
}

static const char *wind_compass(float deg)
{
    static const char *const pts[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };
    int i = (int)lroundf(fmodf(deg + 11.25f, 360.0f) / 22.5f) & 15;
    return pts[i];
}

static float c_to_display(float c)
{
    return s_fahrenheit ? (c * 9.0f / 5.0f + 32.0f) : c;
}

static const char *temp_unit(void)
{
    return s_fahrenheit ? "F" : "C";
}

static void url_encode_query(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '%';
            out[o++] = '2';
            out[o++] = '0';
        } else {
            static const char *hex = "0123456789ABCDEF";
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

// ---- UI -------------------------------------------------------------------

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t stripe)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(T_FACE), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(T_NAVY), 0);
    lv_obj_set_scrollable(card, false);

    lv_obj_t *s = lv_obj_create(card);
    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, 0, 0);
    lv_obj_set_size(s, w, STRIPE_H);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(stripe), 0);
    lv_obj_set_scrollable(s, false);
    return card;
}

static lv_obj_t *make_lbl(lv_obj_t *parent, const lv_font_t *font, uint32_t col, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static void set_hint(const char *text)
{
    if (s_hint) {
        lv_label_set_text(s_hint, text ? text : "");
    }
}

static void render_hourly(void)
{
    if (!s_hour_row) {
        return;
    }
    for (int i = 0; i < HOURLY_N; i++) {
        if (!s_hour_lbl[i]) {
            continue;
        }
        if (i >= s_wx.hourly_n || !s_wx.valid) {
            lv_label_set_text(s_hour_lbl[i], "");
            continue;
        }
        const hour_t *h = &s_wx.hourly[i];
        char line[40];
        snprintf(line, sizeof(line), "%s\n%s%.0f\n%s",
                 h->hour,
                 wmo_icon(h->weather_code),
                 (double)c_to_display(h->temp_c),
                 (h->precip_mm > 0.05f || wmo_is_rain(h->weather_code)) ? "Rn" : "  ");
        lv_label_set_text(s_hour_lbl[i], line);
        lv_obj_set_style_text_color(s_hour_lbl[i],
                                    lv_color_hex((h->precip_mm > 0.05f || wmo_is_rain(h->weather_code))
                                                 ? T_RAIN : T_INK), 0);
    }
}

static void render_pick(void)
{
    for (int i = 0; i < GEO_MAX; i++) {
        if (!s_pick_lbl[i]) {
            continue;
        }
        if (s_mode != UI_CITY_PICK || i >= s_geo_n) {
            lv_label_set_text(s_pick_lbl[i], "");
            continue;
        }
        char line[96];
        snprintf(line, sizeof(line), "%s %.40s%.40s",
                 i == s_geo_sel ? ">" : " ",
                 s_geo[i].name,
                 s_geo[i].detail[0] ? s_geo[i].detail : "");
        lv_label_set_text(s_pick_lbl[i], line);
        lv_obj_set_style_text_color(s_pick_lbl[i],
                                    lv_color_hex(i == s_geo_sel ? T_BLUE : T_DIM), 0);
    }
}

static void render(void)
{
    if (!s_root) {
        return;
    }

    if (s_mode == UI_CITY_PICK) {
        lv_label_set_text(s_loc, "Pick a city");
        lv_label_set_text(s_temp, "");
        lv_label_set_text(s_icon, "");
        lv_label_set_text(s_cond, s_city_query[0] ? s_city_query : "City search");
        lv_label_set_text(s_meta, "");
        lv_label_set_text(s_status, "UP/DOWN  SELECT use  BACK cancel");
        lv_obj_set_style_text_color(s_status, lv_color_hex(T_ORANGE), 0);
        for (int i = 0; i < HOURLY_N; i++) {
            if (s_hour_lbl[i]) {
                lv_label_set_text(s_hour_lbl[i], "");
            }
        }
        render_pick();
        set_hint("UP/DOWN pick  SELECT confirm  CANCEL back");
        return;
    }

    for (int i = 0; i < GEO_MAX; i++) {
        if (s_pick_lbl[i]) {
            lv_label_set_text(s_pick_lbl[i], "");
        }
    }

    char loc[96];
    if (s_city[0]) {
        snprintf(loc, sizeof(loc), "%s%s", s_city, s_using_default ? "  (default)" : "");
    } else {
        snprintf(loc, sizeof(loc), "%s, %s%s",
                 s_lat, s_lon, s_using_default ? "  (default)" : "");
    }
    lv_label_set_text(s_loc, loc);

    if (s_fetch == FETCH_BUSY) {
        lv_label_set_text(s_temp, "...");
        lv_label_set_text(s_icon, "");
        lv_label_set_text(s_cond, s_job == JOB_GEOCODE ? "Searching city" : "Fetching Open-Meteo");
        lv_label_set_text(s_meta, "");
        lv_label_set_text(s_status, LV_SYMBOL_REFRESH "  please wait");
        lv_obj_set_style_text_color(s_status, lv_color_hex(T_ORANGE), 0);
        render_hourly();
        return;
    }

    if (s_fetch == FETCH_FAIL || (s_err[0] && !s_wx.valid)) {
        lv_label_set_text(s_temp, "--");
        lv_label_set_text(s_icon, "!");
        lv_label_set_text(s_cond, s_err[0] ? s_err : "Fetch failed");
        lv_label_set_text(s_meta, "SELECT retry   RIGHT city");
        lv_label_set_text(s_status, LV_SYMBOL_WARNING "  error");
        lv_obj_set_style_text_color(s_status, lv_color_hex(T_RED), 0);
        render_hourly();
        return;
    }

    if (!s_wx.valid) {
        lv_label_set_text(s_temp, "--");
        lv_label_set_text(s_icon, "");
        lv_label_set_text(s_cond, "SELECT to fetch");
        lv_label_set_text(s_meta, "Open-Meteo · no API key");
        if (!wifi_ready()) {
            lv_label_set_text(s_status, LV_SYMBOL_WIFI "  join Wi-Fi");
            lv_obj_set_style_text_color(s_status, lv_color_hex(T_ORANGE), 0);
        } else {
            lv_label_set_text(s_status, LV_SYMBOL_OK "  ready");
            lv_obj_set_style_text_color(s_status, lv_color_hex(T_TEAL), 0);
        }
        render_hourly();
        return;
    }

    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%.0f°%s", (double)c_to_display(s_wx.temp_c), temp_unit());
    lv_label_set_text(s_temp, tbuf);

    const char *ic = wmo_icon(s_wx.weather_code);
    lv_label_set_text(s_icon, ic);
    lv_obj_set_style_text_color(s_icon,
                                lv_color_hex(wmo_is_rain(s_wx.weather_code) ? T_RAIN : T_NAVY), 0);

    char cond[64];
    snprintf(cond, sizeof(cond), "%s  %s", ic, wmo_text(s_wx.weather_code));
    lv_label_set_text(s_cond, cond);

    char meta[160];
    snprintf(meta, sizeof(meta),
             "Feels %.0f°%s   Hum %.0f%%\n"
             "Wind %.0f km/h %s   Rain %.1f mm",
             (double)c_to_display(s_wx.feels_c), temp_unit(),
             (double)s_wx.humidity,
             (double)s_wx.wind_kmh, wind_compass(s_wx.wind_dir),
             (double)s_wx.precip_mm);
    lv_label_set_text(s_meta, meta);

    if (s_wx.time[0]) {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_OK "  %s", s_wx.time);
    } else {
        lv_label_set_text(s_status, LV_SYMBOL_OK "  updated");
    }
    lv_obj_set_style_text_color(s_status, lv_color_hex(T_TEAL), 0);
    render_hourly();
    set_hint("SEL refresh  L C/F  R city  CANCEL exit");
}

static void build_ui(lv_obj_t *root)
{
    battle_paint_stage(root);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(T_SKY), 0);

    s_card = make_card(root, 10, 4, OS_ROOT_W - 20, CONTENT_H - 20, T_BLUE);

    s_loc = make_lbl(s_card, &sol_mono_12, T_DIM, "");
    lv_obj_set_pos(s_loc, 10, STRIPE_H + 4);
    lv_obj_set_width(s_loc, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_loc, LV_LABEL_LONG_MODE_DOTS);

    s_temp = make_lbl(s_card, &sol_font_32, T_INK, "--");
    lv_obj_set_pos(s_temp, 10, 28);

    s_icon = make_lbl(s_card, &sol_font_32, T_NAVY, "");
    lv_obj_set_pos(s_icon, OS_ROOT_W - 72, 28);

    s_cond = make_lbl(s_card, &sol_font_14, T_NAVY, "");
    lv_obj_set_pos(s_cond, 10, 68);
    lv_obj_set_width(s_cond, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_cond, LV_LABEL_LONG_MODE_DOTS);

    s_meta = make_lbl(s_card, &sol_mono_12, T_INK, "");
    lv_obj_set_pos(s_meta, 10, 90);
    lv_obj_set_width(s_meta, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_meta, LV_LABEL_LONG_MODE_WRAP);

    /* Hourly strip — 6 cells across the card. */
    s_hour_row = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_hour_row);
    lv_obj_set_pos(s_hour_row, 6, 128);
    lv_obj_set_size(s_hour_row, OS_ROOT_W - 36, 46);
    lv_obj_set_scrollable(s_hour_row, false);
    {
        int cell_w = (OS_ROOT_W - 36) / HOURLY_N;
        for (int i = 0; i < HOURLY_N; i++) {
            s_hour_lbl[i] = make_lbl(s_hour_row, &sol_mono_12, T_INK, "");
            lv_obj_set_pos(s_hour_lbl[i], i * cell_w, 0);
            lv_obj_set_width(s_hour_lbl[i], cell_w - 2);
            lv_obj_set_style_text_align(s_hour_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(s_hour_lbl[i], LV_LABEL_LONG_MODE_WRAP);
        }
    }

    /* City-pick rows (hidden unless UI_CITY_PICK). */
    for (int i = 0; i < GEO_MAX; i++) {
        s_pick_lbl[i] = make_lbl(s_card, &sol_mono_12, T_DIM, "");
        lv_obj_set_pos(s_pick_lbl[i], 10, 90 + i * 14);
        lv_obj_set_width(s_pick_lbl[i], OS_ROOT_W - 44);
        lv_label_set_long_mode(s_pick_lbl[i], LV_LABEL_LONG_MODE_DOTS);
    }

    s_status = make_lbl(s_card, &sol_mono_12, T_TEAL, "");
    lv_obj_set_pos(s_status, 10, CONTENT_H - 48);
    lv_obj_set_width(s_status, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, OS_ROOT_W, UI_HINT_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(T_CREAM), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(T_NAVY), 0);
    lv_obj_set_scrollable(bar, false);
    s_hint = make_lbl(bar, &sol_mono_12, T_NAVY, "");
    lv_obj_align(s_hint, LV_ALIGN_LEFT_MID, 8, 0);

    s_offline = ui_offline_create(root, 0, CONTENT_H, false);
    set_hint("SEL refresh  L C/F  R city  CANCEL exit");
}

// ---- HTTP workers ---------------------------------------------------------

static bool parse_hourly(cJSON *hourly, wx_t *out)
{
    out->hourly_n = 0;
    if (!cJSON_IsObject(hourly)) {
        return false;
    }
    cJSON *times = cJSON_GetObjectItem(hourly, "time");
    cJSON *temps = cJSON_GetObjectItem(hourly, "temperature_2m");
    cJSON *codes = cJSON_GetObjectItem(hourly, "weather_code");
    cJSON *precs = cJSON_GetObjectItem(hourly, "precipitation");
    if (!cJSON_IsArray(times) || !cJSON_IsArray(temps)) {
        return false;
    }
    int n = cJSON_GetArraySize(times);
    int take = n < HOURLY_N ? n : HOURLY_N;
    for (int i = 0; i < take; i++) {
        hour_t *h = &out->hourly[i];
        memset(h, 0, sizeof(*h));
        const char *t = cJSON_GetStringValue(cJSON_GetArrayItem(times, i));
        if (t) {
            /* ISO "2026-11-25T16:00" → "16h" */
            const char *p = strrchr(t, 'T');
            if (p && strlen(p) >= 3) {
                h->hour[0] = p[1];
                h->hour[1] = p[2];
                h->hour[2] = 'h';
                h->hour[3] = 0;
            } else {
                strlcpy(h->hour, t, sizeof(h->hour));
            }
        }
        h->temp_c = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(temps, i));
        if (cJSON_IsArray(codes)) {
            h->weather_code = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(codes, i));
        }
        if (cJSON_IsArray(precs)) {
            h->precip_mm = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(precs, i));
        }
        out->hourly_n++;
    }
    return out->hourly_n > 0;
}

static bool parse_forecast(const char *json, wx_t *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, err_cap, "bad JSON");
        return false;
    }
    cJSON *reason = cJSON_GetObjectItem(root, "reason");
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "error")) || cJSON_IsString(reason)) {
        const char *r = cJSON_GetStringValue(reason);
        snprintf(err, err_cap, "%s", r && r[0] ? r : "API error");
        cJSON_Delete(root);
        return false;
    }
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (!cJSON_IsObject(cur)) {
        snprintf(err, err_cap, "no current block");
        cJSON_Delete(root);
        return false;
    }
    out->temp_c = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "temperature_2m"));
    out->feels_c = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "apparent_temperature"));
    out->humidity = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "relative_humidity_2m"));
    out->wind_kmh = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "wind_speed_10m"));
    out->wind_dir = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "wind_direction_10m"));
    out->precip_mm = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "precipitation"));
    out->weather_code = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "weather_code"));
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cur, "time"));
    if (t) {
        strlcpy(out->time, t, sizeof(out->time));
    }
    parse_hourly(cJSON_GetObjectItem(root, "hourly"), out);
    out->valid = true;
    cJSON_Delete(root);
    return true;
}

static bool parse_geocode(const char *json, geo_hit_t *hits, int *out_n, char *err, size_t err_cap)
{
    *out_n = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, err_cap, "bad geo JSON");
        return false;
    }
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(err, err_cap, "no cities found");
        cJSON_Delete(root);
        return false;
    }
    int n = cJSON_GetArraySize(results);
    if (n > GEO_MAX) {
        n = GEO_MAX;
    }
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(results, i);
        geo_hit_t *h = &hits[i];
        memset(h, 0, sizeof(*h));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        const char *admin1 = cJSON_GetStringValue(cJSON_GetObjectItem(it, "admin1"));
        const char *country = cJSON_GetStringValue(cJSON_GetObjectItem(it, "country_code"));
        double lat = cJSON_GetNumberValue(cJSON_GetObjectItem(it, "latitude"));
        double lon = cJSON_GetNumberValue(cJSON_GetObjectItem(it, "longitude"));
        strlcpy(h->name, name && name[0] ? name : "?", sizeof(h->name));
        if (admin1 && admin1[0] && country && country[0]) {
            snprintf(h->detail, sizeof(h->detail), ", %s %s", admin1, country);
        } else if (country && country[0]) {
            snprintf(h->detail, sizeof(h->detail), ", %s", country);
        } else if (admin1 && admin1[0]) {
            snprintf(h->detail, sizeof(h->detail), ", %s", admin1);
        }
        snprintf(h->lat, sizeof(h->lat), "%.4f", lat);
        snprintf(h->lon, sizeof(h->lon), "%.4f", lon);
        (*out_n)++;
    }
    cJSON_Delete(root);
    return *out_n > 0;
}

static void worker(void *arg)
{
    job_t job = (job_t)(uintptr_t)arg;
    char url[URL_MAX];
    if (job == JOB_GEOCODE) {
        char enc[CITY_QUERY_MAX * 3];
        url_encode_query(s_city_query, enc, sizeof(enc));
        snprintf(url, sizeof(url),
                 "https://geocoding-api.open-meteo.com/v1/search"
                 "?name=%s&count=%d&language=en&format=json",
                 enc, GEO_MAX);
    } else {
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast"
                 "?latitude=%s&longitude=%s"
                 "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
                 "weather_code,wind_speed_10m,wind_direction_10m,precipitation"
                 "&hourly=temperature_2m,weather_code,precipitation"
                 "&forecast_hours=%d&timezone=auto",
                 s_lat, s_lon, HOURLY_N);
    }

    char *resp = malloc(RESP_CAP);
    if (!resp) {
        strlcpy(s_pending_err, "Out of memory", sizeof(s_pending_err));
        s_fetch = FETCH_FAIL;
        vTaskDelete(NULL);
        return;
    }
    resp[0] = 0;

    os_http_hangup();

    int status = 0;
    esp_err_t err = os_http_request("GET", url, NULL, NULL, resp, RESP_CAP, &status);
    if (err != ESP_OK) {
        snprintf(s_pending_err, sizeof(s_pending_err),
                 err == ESP_ERR_NO_MEM ? "need heap for TLS" : "HTTP failed");
        free(resp);
        s_fetch = FETCH_FAIL;
        vTaskDelete(NULL);
        return;
    }
    if (status / 100 != 2) {
        snprintf(s_pending_err, sizeof(s_pending_err), "HTTP %d", status);
        free(resp);
        s_fetch = FETCH_FAIL;
        vTaskDelete(NULL);
        return;
    }

    if (job == JOB_GEOCODE) {
        if (!parse_geocode(resp, s_pending_geo, &s_pending_geo_n,
                           s_pending_err, sizeof(s_pending_err))) {
            free(resp);
            s_fetch = FETCH_FAIL;
            vTaskDelete(NULL);
            return;
        }
        free(resp);
        s_pending_err[0] = 0;
        s_fetch = FETCH_OK;
        ESP_LOGI(TAG, "geo %d hits for '%s'", s_pending_geo_n, s_city_query);
        vTaskDelete(NULL);
        return;
    }

    wx_t wx;
    if (!parse_forecast(resp, &wx, s_pending_err, sizeof(s_pending_err))) {
        free(resp);
        s_fetch = FETCH_FAIL;
        vTaskDelete(NULL);
        return;
    }
    free(resp);
    s_pending = wx;
    s_pending_err[0] = 0;
    s_fetch = FETCH_OK;
    ESP_LOGI(TAG, "wx %.1fC code %d hourly %d @ %s,%s",
             (double)wx.temp_c, wx.weather_code, wx.hourly_n, s_lat, s_lon);
    vTaskDelete(NULL);
}

static bool start_job(job_t job)
{
    if (s_fetch == FETCH_BUSY) {
        return false;
    }
    if (!wifi_ready()) {
        os_toast("Wi-Fi needed");
        return false;
    }
    if (job == JOB_FORECAST) {
        load_config();
        if (!coords_ok()) {
            strlcpy(s_err, "bad lat/lon", sizeof(s_err));
            os_toast("Set lat/lon first");
            return false;
        }
    } else if (job == JOB_GEOCODE) {
        if (!s_city_query[0]) {
            os_toast("Enter a city");
            return false;
        }
    } else {
        return false;
    }
    s_err[0] = 0;
    s_pending_err[0] = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending_geo_n = 0;
    s_job = job;
    s_fetch = FETCH_BUSY;
    if (xTaskCreate(worker, "weather", WORKER_STACK, (void *)(uintptr_t)job, 3, NULL) != pdPASS) {
        s_fetch = FETCH_FAIL;
        s_job = JOB_NONE;
        strlcpy(s_err, "Task create failed", sizeof(s_err));
        return false;
    }
    return true;
}

static bool start_fetch(void)
{
    return start_job(JOB_FORECAST);
}

static void apply_city(const geo_hit_t *h)
{
    if (!h) {
        return;
    }
    strlcpy(s_lat, h->lat, sizeof(s_lat));
    strlcpy(s_lon, h->lon, sizeof(s_lon));
    strlcpy(s_city, h->name, sizeof(s_city));
    nvs_set("lat", s_lat);
    nvs_set("lon", s_lon);
    nvs_set("city", s_city);
    s_using_default = false;
    s_mode = UI_VIEW;
    s_geo_n = 0;
    os_toast("%s", s_city);
    start_fetch();
}

static void apply_pending(void)
{
    job_t job = s_job;
    if (s_fetch == FETCH_OK) {
        if (job == JOB_GEOCODE) {
            memcpy(s_geo, s_pending_geo, sizeof(s_geo));
            s_geo_n = s_pending_geo_n;
            s_geo_sel = 0;
            s_err[0] = 0;
            s_fetch = FETCH_IDLE;
            s_job = JOB_NONE;
            if (s_geo_n == 1) {
                apply_city(&s_geo[0]);
            } else {
                s_mode = UI_CITY_PICK;
            }
            render();
            return;
        }
        s_wx = s_pending;
        s_err[0] = 0;
        s_fetch = FETCH_IDLE;
        s_job = JOB_NONE;
        render();
        return;
    }
    if (s_fetch == FETCH_FAIL) {
        strlcpy(s_err, s_pending_err[0] ? s_pending_err : "Fetch failed", sizeof(s_err));
        s_fetch = FETCH_IDLE;
        s_job = JOB_NONE;
        render();
        os_toast("%s", s_err);
    }
}

static void open_city_kb(void)
{
    if (s_kb) {
        return;
    }
    s_kb = ui_kb_create(s_root, "City name", CITY_QUERY_MAX - 1);
}

// ---- console --------------------------------------------------------------

static int cmd_weather(int argc, char **argv)
{
    load_config();
    if (argc < 2 || !strcmp(argv[1], "status")) {
        printf("city  %s%s\n", s_city[0] ? s_city : "(none)",
               s_using_default ? " (default London)" : "");
        printf("lat   %s\n", s_lat);
        printf("lon   %s\n", s_lon);
        printf("units %s\n", s_fahrenheit ? "F" : "C");
        printf("fetch %d job %d valid %d hourly %d\n",
               (int)s_fetch, (int)s_job, s_wx.valid ? 1 : 0, s_wx.hourly_n);
        if (s_wx.valid) {
            printf("temp  %.1f C  feels %.1f C  hum %.0f%%\n",
                   (double)s_wx.temp_c, (double)s_wx.feels_c, (double)s_wx.humidity);
            printf("wind  %.0f km/h %s  code %d %s (%s)\n",
                   (double)s_wx.wind_kmh, wind_compass(s_wx.wind_dir),
                   s_wx.weather_code, wmo_icon(s_wx.weather_code),
                   wmo_text(s_wx.weather_code));
            for (int i = 0; i < s_wx.hourly_n; i++) {
                printf("  %s  %s %.0fC  %.1fmm\n",
                       s_wx.hourly[i].hour,
                       wmo_icon(s_wx.hourly[i].weather_code),
                       (double)s_wx.hourly[i].temp_c,
                       (double)s_wx.hourly[i].precip_mm);
            }
            if (s_wx.time[0]) {
                printf("time  %s\n", s_wx.time);
            }
        }
        if (s_err[0]) {
            printf("err   %s\n", s_err);
        }
        return 0;
    }
    if (!strcmp(argv[1], "city") && argc >= 3) {
        strlcpy(s_city_query, argv[2], sizeof(s_city_query));
        for (int i = 3; i < argc; i++) {
            size_t n = strlen(s_city_query);
            if (n + 1 < sizeof(s_city_query)) {
                s_city_query[n++] = ' ';
                s_city_query[n] = 0;
            }
            strlcat(s_city_query, argv[i], sizeof(s_city_query));
        }
        if (!start_job(JOB_GEOCODE)) {
            return 1;
        }
        printf("searching '%s'\n", s_city_query);
        return 0;
    }
    if (!strcmp(argv[1], "lat") && argc >= 3) {
        float v;
        if (!parse_coord(argv[2], &v) || v < -90.0f || v > 90.0f) {
            printf("lat must be -90..90\n");
            return 1;
        }
        if (!nvs_set("lat", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_lat, argv[2], sizeof(s_lat));
        s_using_default = false;
        printf("lat %s\n", s_lat);
        return 0;
    }
    if (!strcmp(argv[1], "lon") && argc >= 3) {
        float v;
        if (!parse_coord(argv[2], &v) || v < -180.0f || v > 180.0f) {
            printf("lon must be -180..180\n");
            return 1;
        }
        if (!nvs_set("lon", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_lon, argv[2], sizeof(s_lon));
        s_using_default = false;
        printf("lon %s\n", s_lon);
        return 0;
    }
    if (!strcmp(argv[1], "loc") && argc >= 4) {
        float lat, lon;
        if (!parse_coord(argv[2], &lat) || lat < -90.0f || lat > 90.0f ||
            !parse_coord(argv[3], &lon) || lon < -180.0f || lon > 180.0f) {
            printf("usage: weather loc <lat> <lon>\n");
            return 1;
        }
        if (!nvs_set("lat", argv[2]) || !nvs_set("lon", argv[3])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_lat, argv[2], sizeof(s_lat));
        strlcpy(s_lon, argv[3], sizeof(s_lon));
        s_using_default = false;
        printf("loc %s, %s\n", s_lat, s_lon);
        return 0;
    }
    if (!strcmp(argv[1], "units") && argc >= 3) {
        if (!strcmp(argv[2], "f") || !strcmp(argv[2], "F") || !strcmp(argv[2], "fahrenheit")) {
            s_fahrenheit = true;
            nvs_set("units", "f");
            printf("units F\n");
        } else if (!strcmp(argv[2], "c") || !strcmp(argv[2], "C") || !strcmp(argv[2], "celsius")) {
            s_fahrenheit = false;
            nvs_set("units", "c");
            printf("units C\n");
        } else {
            printf("usage: weather units c|f\n");
            return 1;
        }
        if (s_root) {
            render();
        }
        return 0;
    }
    if (!strcmp(argv[1], "go")) {
        if (!start_fetch()) {
            return 1;
        }
        printf("fetching %s,%s\n", s_lat, s_lon);
        return 0;
    }
    if (!strcmp(argv[1], "clear")) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        s_fahrenheit = false;
        s_city[0] = 0;
        load_config();
        printf("cleared NVS weather config (SD files untouched)\n");
        return 0;
    }
    printf("weather [status|city <name>|lat <v>|lon <v>|loc <lat> <lon>|units c|f|go|clear]\n");
    return 1;
}

static void weather_register_cmd(void)
{
    static bool once;
    if (once) {
        return;
    }
    once = true;
    os_cmd_register("weather",
                    "weather [status|city|lat|lon|loc|units|go|clear] - Open-Meteo forecast",
                    cmd_weather);
}

__attribute__((constructor)) static void weather_ctor(void)
{
    weather_register_cmd();
}

// ---- app lifecycle --------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_offline = s_hint = s_card = NULL;
    s_loc = s_temp = s_cond = s_meta = s_status = s_icon = s_hour_row = NULL;
    memset(s_hour_lbl, 0, sizeof(s_hour_lbl));
    memset(s_pick_lbl, 0, sizeof(s_pick_lbl));
    s_kb = NULL;
    s_mode = UI_VIEW;
    s_fetch = FETCH_IDLE;
    s_job = JOB_NONE;
    s_err[0] = 0;
    s_geo_n = 0;
    memset(&s_wx, 0, sizeof(s_wx));
    load_config();
    build_ui(root);
    render();
    if (wifi_ready()) {
        start_fetch();
        render();
    }
}

static void leave(void)
{
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    s_root = s_offline = s_hint = s_card = NULL;
    s_loc = s_temp = s_cond = s_meta = s_status = s_icon = s_hour_row = NULL;
    memset(s_hour_lbl, 0, sizeof(s_hour_lbl));
    memset(s_pick_lbl, 0, sizeof(s_pick_lbl));
    s_mode = UI_VIEW;
}

static void tick(void)
{
    if (!s_root) {
        return;
    }
    if (s_kb) {
        return;
    }
    ui_offline_update(s_offline);
    if (wifi_ready() && s_offline) {
        lv_obj_set_hidden(s_offline, true);
    }
    if (s_fetch == FETCH_BUSY) {
        render();
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
            const char *t = ui_kb_text(s_kb);
            strlcpy(s_city_query, t ? t : "", sizeof(s_city_query));
            trim_inplace(s_city_query);
            ui_kb_delete(s_kb);
            s_kb = NULL;
            if (!s_city_query[0]) {
                set_hint("SEL refresh  L C/F  R city  CANCEL exit");
                return true;
            }
            start_job(JOB_GEOCODE);
            render();
            return true;
        }
        case UI_KB_CANCEL:
            ui_kb_delete(s_kb);
            s_kb = NULL;
            set_hint("SEL refresh  L C/F  R city  CANCEL exit");
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

    if (s_mode == UI_CITY_PICK) {
        if (k == OS_KEY_UP && !repeat) {
            if (s_geo_sel > 0) {
                s_geo_sel--;
            } else {
                s_geo_sel = s_geo_n - 1;
            }
            render();
            return true;
        }
        if (k == OS_KEY_DOWN && !repeat) {
            s_geo_sel = (s_geo_sel + 1) % (s_geo_n > 0 ? s_geo_n : 1);
            render();
            return true;
        }
        if (k == OS_KEY_SELECT && !repeat) {
            if (s_geo_sel >= 0 && s_geo_sel < s_geo_n) {
                apply_city(&s_geo[s_geo_sel]);
                render();
            }
            return true;
        }
        if (k == OS_KEY_BACK && !repeat) {
            s_mode = UI_VIEW;
            s_geo_n = 0;
            render();
            return true;
        }
        return true;
    }

    if (k == OS_KEY_SELECT && !repeat) {
        if (s_fetch == FETCH_BUSY) {
            os_toast("Busy...");
            return true;
        }
        start_fetch();
        render();
        return true;
    }
    if (k == OS_KEY_LEFT && !repeat) {
        s_fahrenheit = !s_fahrenheit;
        nvs_set("units", s_fahrenheit ? "f" : "c");
        render();
        os_toast("%s", s_fahrenheit ? "Fahrenheit" : "Celsius");
        return true;
    }
    if (k == OS_KEY_RIGHT && !repeat) {
        if (s_fetch == FETCH_BUSY) {
            os_toast("Busy...");
            return true;
        }
        open_city_kb();
        return true;
    }
    return false;
}

const os_app_t app_weather = {
    .id = "weather",
    .name = "Weather",
    .symbol = LV_SYMBOL_REFRESH,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
