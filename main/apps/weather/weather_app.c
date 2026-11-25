// Open-Meteo current weather for BP26.
//
// Fetches https://api.open-meteo.com/v1/forecast over Wi-Fi with os_http
// (no API key). Lat/lon from NVS (console `weather`) or SD files. UI uses the
// pocket-battle palette on the 320×240 panel.

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
#define URL_MAX         320
#define RESP_CAP        4096
#define WORKER_STACK    8192
#define CONTENT_H       (OS_ROOT_H - UI_HINT_H)

/* Default: London (documented). Override via NVS / SD. */
#define DEFAULT_LAT     "51.5074"
#define DEFAULT_LON     "-0.1278"

typedef enum {
    FETCH_IDLE = 0,
    FETCH_BUSY,
    FETCH_OK,
    FETCH_FAIL,
} fetch_t;

typedef struct {
    float temp_c;
    float feels_c;
    float humidity;
    float wind_kmh;
    float wind_dir;
    float precip_mm;
    int weather_code;
    char time[40];
    bool valid;
} wx_t;

static lv_obj_t *s_root, *s_offline, *s_hint;
static lv_obj_t *s_card, *s_loc, *s_temp, *s_cond, *s_meta, *s_status;

static char s_lat[COORD_MAX];
static char s_lon[COORD_MAX];
static bool s_fahrenheit;
static bool s_using_default;

static wx_t s_wx;
static volatile fetch_t s_fetch;
static char s_err[96];
static wx_t s_pending;
static char s_pending_err[96];

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
    s_lat[0] = s_lon[0] = 0;
    s_using_default = false;
    bool have_lat = nvs_get("lat", s_lat, sizeof(s_lat));
    bool have_lon = nvs_get("lon", s_lon, sizeof(s_lon));
    if (!have_lat) {
        have_lat = read_file_line(LAT_FILE, s_lat, sizeof(s_lat));
    }
    if (!have_lon) {
        have_lon = read_file_line(LON_FILE, s_lon, sizeof(s_lon));
    }
    if ((!have_lat || !have_lon)) {
        char buf[80];
        if (read_file_line(LOC_FILE, buf, sizeof(buf))) {
            // weather.loc: "lat,lon" or "lat lon"
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

// ---- UI -------------------------------------------------------------------

static void set_hint(const char *text)
{
    battle_set_hint(s_hint, text);
}

static void render(void)
{
    if (!s_root) {
        return;
    }

    char loc[96];
    snprintf(loc, sizeof(loc), "%s, %s%s",
             s_lat, s_lon, s_using_default ? "  (default)" : "");
    lv_label_set_text(s_loc, loc);

    if (s_fetch == FETCH_BUSY) {
        lv_label_set_text(s_temp, "...");
        lv_label_set_text(s_cond, "Fetching Open-Meteo");
        lv_label_set_text(s_meta, "");
        lv_label_set_text(s_status, LV_SYMBOL_REFRESH "  please wait");
        lv_obj_set_style_text_color(s_status, lv_color_hex(T_ORANGE), 0);
        return;
    }

    if (s_fetch == FETCH_FAIL || (s_err[0] && !s_wx.valid)) {
        lv_label_set_text(s_temp, "--");
        lv_label_set_text(s_cond, s_err[0] ? s_err : "Fetch failed");
        lv_label_set_text(s_meta, "SELECT retry");
        lv_label_set_text(s_status, LV_SYMBOL_WARNING "  error");
        lv_obj_set_style_text_color(s_status, lv_color_hex(T_RED), 0);
        return;
    }

    if (!s_wx.valid) {
        lv_label_set_text(s_temp, "--");
        lv_label_set_text(s_cond, "SELECT to fetch");
        lv_label_set_text(s_meta, "Open-Meteo · no API key");
        if (!wifi_ready()) {
            lv_label_set_text(s_status, LV_SYMBOL_WIFI "  join Wi-Fi");
            lv_obj_set_style_text_color(s_status, lv_color_hex(T_ORANGE), 0);
        } else {
            lv_label_set_text(s_status, LV_SYMBOL_OK "  ready");
            lv_obj_set_style_text_color(s_status, lv_color_hex(T_TEAL), 0);
        }
        return;
    }

    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%.0f°%s", (double)c_to_display(s_wx.temp_c), temp_unit());
    lv_label_set_text(s_temp, tbuf);
    lv_label_set_text(s_cond, wmo_text(s_wx.weather_code));

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
}

static void build_ui(lv_obj_t *root)
{
    battle_paint_stage(root);

    s_card = battle_card(root, 10, 8, OS_ROOT_W - 20, CONTENT_H - 28, T_BLUE);

    s_loc = battle_label(s_card, &sol_mono_12, T_DIM, "");
    lv_obj_set_pos(s_loc, 10, BATTLE_STRIPE_H + 6);
    lv_obj_set_width(s_loc, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_loc, LV_LABEL_LONG_MODE_DOTS);

    s_temp = battle_label(s_card, &sol_font_32, T_INK, "--");
    lv_obj_set_pos(s_temp, 10, 36);

    s_cond = battle_label(s_card, &sol_font_14, T_NAVY, "");
    lv_obj_set_pos(s_cond, 10, 84);
    lv_obj_set_width(s_cond, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_cond, LV_LABEL_LONG_MODE_DOTS);

    s_meta = battle_label(s_card, &sol_mono_12, T_INK, "");
    lv_obj_set_pos(s_meta, 10, 110);
    lv_obj_set_width(s_meta, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_meta, LV_LABEL_LONG_MODE_WRAP);

    s_status = battle_label(s_card, &sol_mono_12, T_TEAL, "");
    lv_obj_set_pos(s_status, 10, CONTENT_H - 56);
    lv_obj_set_width(s_status, OS_ROOT_W - 44);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);

    s_hint = battle_hint(root, "");
    s_offline = ui_offline_create(root, 0, CONTENT_H, false);
    set_hint("SELECT refresh  LEFT C/F  CANCEL exit");
}

// ---- HTTP worker ----------------------------------------------------------

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
    out->valid = true;
    cJSON_Delete(root);
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    char url[URL_MAX];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
             "weather_code,wind_speed_10m,wind_direction_10m,precipitation"
             "&timezone=auto",
             s_lat, s_lon);

    char *resp = malloc(RESP_CAP);
    if (!resp) {
        strlcpy(s_pending_err, "Out of memory", sizeof(s_pending_err));
        s_fetch = FETCH_FAIL;
        vTaskDelete(NULL);
        return;
    }
    resp[0] = 0;

    // Free badge-server keep-alive so TLS can talk to Open-Meteo.
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
    ESP_LOGI(TAG, "wx %.1fC code %d @ %s,%s", (double)wx.temp_c, wx.weather_code, s_lat, s_lon);
    vTaskDelete(NULL);
}

static bool start_fetch(void)
{
    if (s_fetch == FETCH_BUSY) {
        return false;
    }
    load_config();
    if (!coords_ok()) {
        strlcpy(s_err, "bad lat/lon", sizeof(s_err));
        os_toast("Set lat/lon first");
        return false;
    }
    if (!wifi_ready()) {
        os_toast("Wi-Fi needed");
        return false;
    }
    s_err[0] = 0;
    s_pending_err[0] = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    s_fetch = FETCH_BUSY;
    if (xTaskCreate(worker, "weather", WORKER_STACK, NULL, 3, NULL) != pdPASS) {
        s_fetch = FETCH_FAIL;
        strlcpy(s_err, "Task create failed", sizeof(s_err));
        return false;
    }
    return true;
}

static void apply_pending(void)
{
    if (s_fetch == FETCH_OK) {
        s_wx = s_pending;
        s_err[0] = 0;
        s_fetch = FETCH_IDLE;
        render();
        return;
    }
    if (s_fetch == FETCH_FAIL) {
        strlcpy(s_err, s_pending_err[0] ? s_pending_err : "Fetch failed", sizeof(s_err));
        s_fetch = FETCH_IDLE;
        render();
        os_toast("%s", s_err);
    }
}

// ---- console --------------------------------------------------------------

static int cmd_weather(int argc, char **argv)
{
    load_config();
    if (argc < 2 || !strcmp(argv[1], "status")) {
        printf("lat   %s%s\n", s_lat, s_using_default ? " (default London)" : "");
        printf("lon   %s\n", s_lon);
        printf("units %s\n", s_fahrenheit ? "F" : "C");
        printf("fetch %d valid %d\n", (int)s_fetch, s_wx.valid ? 1 : 0);
        if (s_wx.valid) {
            printf("temp  %.1f C  feels %.1f C  hum %.0f%%\n",
                   (double)s_wx.temp_c, (double)s_wx.feels_c, (double)s_wx.humidity);
            printf("wind  %.0f km/h %s  code %d (%s)\n",
                   (double)s_wx.wind_kmh, wind_compass(s_wx.wind_dir),
                   s_wx.weather_code, wmo_text(s_wx.weather_code));
            if (s_wx.time[0]) {
                printf("time  %s\n", s_wx.time);
            }
        }
        if (s_err[0]) {
            printf("err   %s\n", s_err);
        }
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
        load_config();
        printf("cleared NVS weather config (SD files untouched)\n");
        return 0;
    }
    printf("weather [status|lat <v>|lon <v>|loc <lat> <lon>|units c|f|go|clear]\n");
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
                    "weather [status|lat|lon|loc|units|go|clear] - Open-Meteo forecast",
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
    s_loc = s_temp = s_cond = s_meta = s_status = NULL;
    s_fetch = FETCH_IDLE;
    s_err[0] = 0;
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
    s_root = s_offline = s_hint = s_card = NULL;
    s_loc = s_temp = s_cond = s_meta = s_status = NULL;
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
    if (s_fetch == FETCH_BUSY) {
        render();
    }
    if (s_fetch == FETCH_OK || s_fetch == FETCH_FAIL) {
        apply_pending();
    }
}

static bool key(os_key_t k, bool repeat)
{
    if (ui_offline_key(s_offline, k)) {
        return true;
    }
    if (s_offline && !lv_obj_is_hidden(s_offline)) {
        return k != OS_KEY_BACK;
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
    return false;
}

const os_app_t app_weather = {
    .id = "weather",
    .name = "Weather",
    .symbol = LV_SYMBOL_REFRESH,
    .accent = T_BLUE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
