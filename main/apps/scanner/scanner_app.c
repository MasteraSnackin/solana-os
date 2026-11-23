// Nearby radios: live Wi-Fi APs and BLE advertisements. Does not call
// os_background_pause — this toy needs the radios. Emulators pause them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core/os.h"
#include "core/os_ble.h"
#include "core/os_mem.h"
#include "core/os_wifi.h"
#include "core/ui.h"

static const char *TAG = "scanner";

#define WIFI_MAX    24
#define BLE_MAX     24
#define VIEW_MAX    (WIFI_MAX + BLE_MAX)
#define SCAN_MAX    32
#define BLE_TTL_US  (12000000LL)

typedef struct {
    char title[33];
    char sub[48];
    int8_t rssi;
    bool ble;
    int64_t seen;
    uint8_t addr[6];
} hit_t;

static lv_obj_t *s_status, *s_detail;
static ui_list_t *s_list;
static hit_t *s_wifi, *s_ble, *s_view;
static int s_nwifi, s_nble, s_nview;
static bool s_wifi_scanning;
static int64_t s_retry_wifi_at;
static volatile int s_ble_state;     // 0 idle, 1 starting, 2 on, 3 fail
static esp_err_t s_ble_err;
static char s_ble_owner;
static bool s_alive;

static uint32_t rssi_color(int rssi)
{
    return rssi >= -60 ? COL_GREEN : rssi >= -75 ? COL_YELLOW : COL_ORANGE;
}

static int by_rssi(const void *a, const void *b)
{
    return ((const hit_t *)b)->rssi - ((const hit_t *)a)->rssi;
}

static void rebuild_view(void)
{
    if (!s_wifi || !s_ble || !s_view) {
        return;
    }
    int n = 0;
    for (int i = 0; i < s_nwifi && n < VIEW_MAX; i++) {
        s_view[n++] = s_wifi[i];
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_nble && n < VIEW_MAX; i++) {
        if (now - s_ble[i].seen > BLE_TTL_US) {
            continue;
        }
        s_view[n++] = s_ble[i];
    }
    qsort(s_view, n, sizeof(s_view[0]), by_rssi);
    s_nview = n;
    if (s_list) {
        ui_list_set_count(s_list, s_nview);
    }
}

static void row(int i, ui_row_t *r, void *ctx)
{
    (void)ctx;
    const hit_t *h = &s_view[i];
    strlcpy(r->title, h->title[0] ? h->title : "(no name)", sizeof(r->title));
    strlcpy(r->sub, h->sub, sizeof(r->sub));
    snprintf(r->right, sizeof(r->right), "%d dBm", h->rssi);
    r->right_color = rssi_color(h->rssi);
    r->title_color = h->ble ? COL_AQUA : COL_TEXT;
}

static void render_status(void)
{
    const char *ble = s_ble_state == 2 ? "on" : s_ble_state == 1 ? "starting" :
                      s_ble_state == 3 ? esp_err_to_name(s_ble_err) : "off";
    lv_label_set_text_fmt(s_status, LV_SYMBOL_WIFI " %d   " LV_SYMBOL_BLUETOOTH " %d", s_nwifi, s_nble);
    if (s_wifi_scanning) {
        lv_label_set_text_fmt(s_detail, "Scanning Wi-Fi 2.4+5 GHz  BLE %s", ble);
    } else {
        lv_label_set_text_fmt(s_detail, "SELECT refresh  BLE %s  BOOT exit", ble);
    }
}

static void start_wifi_scan(void)
{
    if (s_wifi_scanning) {
        return;
    }
    esp_err_t err = os_wifi_scan_start();
    if (err == ESP_OK) {
        s_wifi_scanning = true;
        s_retry_wifi_at = 0;
        ui_list_set_empty(s_list, "Scanning Wi-Fi and BLE...");
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_retry_wifi_at = esp_timer_get_time() + 1500000;
        ESP_LOGW(TAG, "wifi scan: %s", esp_err_to_name(err));
    } else {
        s_retry_wifi_at = 0;
        ui_list_set_empty(s_list, "Wi-Fi is off on this boot.\nBLE still scans if SRAM allows.");
        ESP_LOGW(TAG, "wifi scan: %s", esp_err_to_name(err));
    }
    render_status();
}

static void collect_wifi(void)
{
    wifi_ap_record_t *recs = malloc(SCAN_MAX * sizeof(*recs));
    int n = recs ? os_wifi_scan_results(recs, SCAN_MAX) : 0;
    s_nwifi = 0;
    for (int i = 0; i < n && s_nwifi < WIFI_MAX; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        bool five = recs[i].primary > 14;
        hit_t *dup = NULL;
        for (int j = 0; ssid[0] && j < s_nwifi; j++) {
            if (!strcmp(s_wifi[j].title, ssid) &&
                (strstr(s_wifi[j].sub, "5 GHz") != NULL) == five) {
                dup = &s_wifi[j];
                break;
            }
        }
        if (dup) {
            if (recs[i].rssi > dup->rssi) {
                dup->rssi = recs[i].rssi;
            }
            continue;
        }
        hit_t *t = &s_wifi[s_nwifi++];
        memset(t, 0, sizeof(*t));
        strlcpy(t->title, ssid[0] ? ssid : "(hidden)", sizeof(t->title));
        t->rssi = recs[i].rssi;
        snprintf(t->sub, sizeof(t->sub), "Wi-Fi  ch %d  %s", recs[i].primary,
                 recs[i].primary > 14 ? "5 GHz" : "2.4 GHz");
        t->seen = esp_timer_get_time();
    }
    free(recs);
    ui_list_set_empty(s_list, "Nothing nearby.\nSELECT to scan again.");
    rebuild_view();
}

static void ingest_ble(const os_ble_ev_t *ev)
{
    int slot = -1;
    for (int i = 0; i < s_nble; i++) {
        if (!memcmp(s_ble[i].addr, ev->addr.val, 6)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s_nble >= BLE_MAX) {
            int weakest = 0;
            for (int i = 1; i < s_nble; i++) {
                if (s_ble[i].rssi < s_ble[weakest].rssi) {
                    weakest = i;
                }
            }
            if (ev->rssi < s_ble[weakest].rssi) {
                return;
            }
            slot = weakest;
        } else {
            slot = s_nble++;
        }
    }
    hit_t *t = &s_ble[slot];
    memset(t, 0, sizeof(*t));
    memcpy(t->addr, ev->addr.val, 6);
    t->ble = true;
    t->rssi = ev->rssi;
    t->seen = esp_timer_get_time();
    if (ev->name[0]) {
        strlcpy(t->title, ev->name, sizeof(t->title));
    } else {
        snprintf(t->title, sizeof(t->title), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ev->addr.val[0], ev->addr.val[1], ev->addr.val[2],
                 ev->addr.val[3], ev->addr.val[4], ev->addr.val[5]);
    }
    snprintf(t->sub, sizeof(t->sub), "BLE  %02X:%02X:%02X:%02X:%02X:%02X%s",
             ev->addr.val[0], ev->addr.val[1], ev->addr.val[2],
             ev->addr.val[3], ev->addr.val[4], ev->addr.val[5],
             ev->connectable ? "  conn" : "");
}

static void drain_ble(void)
{
    if (s_ble_state != 2) {
        return;
    }
    os_ble_ev_t ev;
    bool any = false;
    while (os_ble_next(&s_ble_owner, &ev)) {
        if (ev.kind == OS_BLE_EV_SCAN) {
            ingest_ble(&ev);
            any = true;
        }
        free(ev.data);
    }
    if (any) {
        rebuild_view();
    }
}

static void ble_boot(void *arg)
{
    (void)arg;
    if (!s_alive) {
        s_ble_state = 3;
        vTaskDelete(NULL);
        return;
    }
    esp_err_t e = os_ble_start(NULL, &s_ble_owner);
    if (e == ESP_OK && s_alive) {
        os_ble_scan_t sc = { .active = true, .seconds = 0, .duty = 40 };
        e = os_ble_scan(&s_ble_owner, &sc);
    }
    if (!s_alive) {
        os_ble_stop(&s_ble_owner);
        s_ble_state = 0;
        vTaskDelete(NULL);
        return;
    }
    s_ble_err = e;
    s_ble_state = (e == ESP_OK) ? 2 : 3;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "ble: %s (SRAM %u free)", esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(OS_MEM_SRAM));
    }
    vTaskDelete(NULL);
}

static void *psram(size_t n)
{
    void *p = heap_caps_calloc(1, n, OS_MEM_PSRAM);
    return p ? p : calloc(1, n);
}

static void enter(lv_obj_t *root)
{
    s_nwifi = s_nble = s_nview = 0;
    s_wifi_scanning = false;
    s_retry_wifi_at = 0;
    s_ble_err = ESP_OK;
    s_ble_state = 1;
    s_alive = true;
    s_wifi = psram(WIFI_MAX * sizeof(*s_wifi));
    s_ble = psram(BLE_MAX * sizeof(*s_ble));
    s_view = psram(VIEW_MAX * sizeof(*s_view));
    if (!s_wifi || !s_ble || !s_view) {
        ui_label(root, &sol_font_14, COL_ERR, "Not enough memory");
        ui_hint(root, "BOOT exit");
        return;
    }

    lv_obj_t *head = ui_panel(root, 4, 4, OS_ROOT_W - 8, 42);
    lv_obj_set_style_pad_ver(head, 3, 0);
    s_status = ui_label(head, &sol_font_14, COL_TEXT, "");
    lv_obj_set_width(s_status, OS_ROOT_W - 24);
    s_detail = ui_label(head, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_detail, 0, 18);
    s_list = ui_list_create(root, 50, OS_ROOT_H - 50 - UI_HINT_H, row, NULL);
    ui_list_set_empty(s_list, "Scanning Wi-Fi and BLE...");
    ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll  SELECT refresh  BOOT exit");
    render_status();
    start_wifi_scan();
    if (xTaskCreateWithCaps(ble_boot, "scanble", 4096, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (xTaskCreate(ble_boot, "scanble", 4096, NULL, 2, NULL) != pdPASS) {
            s_ble_err = ESP_ERR_NO_MEM;
            s_ble_state = 3;
        }
    }
}

static void on_leave(void)
{
    s_alive = false;
    os_ble_stop(&s_ble_owner);
    s_ble_state = 0;
    s_list = NULL;
    free(s_wifi);
    free(s_ble);
    free(s_view);
    s_wifi = s_ble = s_view = NULL;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s_list) {
        return false;
    }
    if (ui_list_key(s_list, k, repeat)) {
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        start_wifi_scan();
        return true;
    }
    return false;       // CANCEL / unhandled: OS may exit. HOME is BOOT.
}

static void on_tick(void)
{
    if (!s_list) {
        return;
    }
    if (s_wifi_scanning && os_wifi_scan_done()) {
        s_wifi_scanning = false;
        collect_wifi();
        render_status();
    }
    if (s_retry_wifi_at && esp_timer_get_time() > s_retry_wifi_at) {
        start_wifi_scan();
    }
    drain_ble();
    static int n;
    if (++n % 5 == 0) {
        rebuild_view();
        render_status();
    }
}

const os_app_t app_scanner = {
    .id = "scanner",
    .name = "Scanner",
    .symbol = LV_SYMBOL_GPS,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
