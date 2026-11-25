// Wi-Fi Speed Check: download (and optional upload) throughput against a
// public HTTP(S) endpoint — default Cloudflare speed.cloudflare.com — or a
// URL set over the console / SD. Shows Mbps, latency, RSSI, and SSID.
//
// Needs Wi-Fi up. Reuses ui_offline when disconnected. HTTP runs on a worker
// task with its own esp_http_client (after os_http_hangup) so streaming a
// multi-MB body does not go through the badge-server keep-alive client.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_cmd.h"
#include "core/os_http.h"
#include "core/os_wifi.h"
#include "core/ui.h"

#ifndef SOLANA_OS_EMULATOR
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#endif

static const char *TAG = "wifispeed";

#define NVS_NS          "wifispeed"
#define URL_FILE        BSP_SD_MOUNT "/wifispeed.url"
#define URL_MAX         192
#define HOST_MAX        48
#define CONTENT_H       (OS_ROOT_H - UI_HINT_H)
#define DEFAULT_BYTES   (1000 * 1000)   // 1 MB download sample
#define UPLOAD_BYTES    (250 * 1000)    // 250 KB upload sample
#define CHUNK           2048
#define HTTP_TIMEOUT_MS 45000

#define DEFAULT_DOWN_FMT "https://speed.cloudflare.com/__down?bytes=%u"
#define DEFAULT_UP_URL   "https://speed.cloudflare.com/__up"
#define DEFAULT_LAT_URL  "https://speed.cloudflare.com/__down?bytes=0"

typedef enum {
    PHASE_IDLE = 0,
    PHASE_LATENCY,
    PHASE_DOWNLOAD,
    PHASE_UPLOAD,
    PHASE_DONE,
    PHASE_FAIL,
} phase_t;

typedef struct {
    phase_t phase;
    char err[96];
    int latency_ms;         // -1 = not measured
    float down_mbps;        // <0 = not measured
    float up_mbps;
    size_t bytes_done;
    size_t bytes_total;
    bool do_upload;
} result_t;

static lv_obj_t *s_root, *s_ssid, *s_rssi, *s_host, *s_lat, *s_down, *s_up, *s_prog, *s_offline, *s_hint;
static bool s_busy;
static bool s_want_upload = true;
static volatile bool s_cancel;
static char s_url[URL_MAX];             // optional override (download URL)
static uint32_t s_dl_bytes = DEFAULT_BYTES;
static result_t s_last;
static result_t s_live;                 // worker → tick via copy under busy

// ---- config ---------------------------------------------------------------

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
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
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

static bool cfg_get_u32(const char *key, uint32_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u32(h, key, out);
    nvs_close(h);
    return err == ESP_OK;
}

static bool cfg_set_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static void load_config(void)
{
    s_url[0] = 0;
    if (!nvs_get("url", s_url, sizeof(s_url))) {
        read_file_line(URL_FILE, s_url, sizeof(s_url));
    }
    uint32_t b = DEFAULT_BYTES;
    if (cfg_get_u32("bytes", &b) && b >= 10000 && b <= 8 * 1000 * 1000) {
        s_dl_bytes = b;
    } else {
        s_dl_bytes = DEFAULT_BYTES;
    }
    uint32_t up = 1;
    if (cfg_get_u32("upload", &up)) {
        s_want_upload = up != 0;
    }
}

static void host_of(const char *url, char *out, size_t n)
{
    out[0] = 0;
    if (!url || !url[0]) {
        strlcpy(out, "speed.cloudflare.com", n);
        return;
    }
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    snprintf(out, n, "%.*s", (int)strcspn(p, "/:?"), p);
}

static void build_down_url(char *out, size_t cap)
{
    if (s_url[0]) {
        strlcpy(out, s_url, cap);
        return;
    }
    snprintf(out, cap, DEFAULT_DOWN_FMT, (unsigned)s_dl_bytes);
}

static bool using_cloudflare(void)
{
    if (!s_url[0]) {
        return true;
    }
    return strstr(s_url, "speed.cloudflare.com") != NULL;
}

// ---- UI -------------------------------------------------------------------

static void set_text(lv_obj_t *lab, const char *text)
{
    if (lab && strcmp(lv_label_get_text(lab), text)) {
        lv_label_set_text(lab, text);
    }
}

static void fmt_mbps(char *out, size_t n, float mbps)
{
    if (mbps < 0) {
        strlcpy(out, "--", n);
    } else if (mbps < 10) {
        snprintf(out, n, "%.2f Mbps", (double)mbps);
    } else {
        snprintf(out, n, "%.1f Mbps", (double)mbps);
    }
}

static void render_link(void)
{
    if (!s_ssid) {
        return;
    }
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    char line[64];
    if (w.state == OS_WIFI_CONNECTED && w.ssid[0]) {
        snprintf(line, sizeof(line), LV_SYMBOL_WIFI "  %s", w.ssid);
        set_text(s_ssid, line);
        snprintf(line, sizeof(line), "%d dBm   %s", w.rssi, w.ip);
        set_text(s_rssi, line);
        lv_obj_set_style_text_color(s_rssi, ui_col(w.rssi >= -60 ? COL_GREEN : w.rssi >= -75 ? COL_YELLOW : COL_ORANGE), 0);
    } else {
        set_text(s_ssid, LV_SYMBOL_WIFI "  (not joined)");
        set_text(s_rssi, "");
    }
    char host[HOST_MAX], url[URL_MAX + 32];
    build_down_url(url, sizeof(url));
    host_of(url, host, sizeof(host));
    snprintf(line, sizeof(line), "via %s%s", host, s_want_upload && using_cloudflare() ? "  +up" : "");
    set_text(s_host, line);
}

static void render_results(const result_t *r)
{
    if (!s_lat) {
        return;
    }
    char line[48];
    if (r->latency_ms < 0) {
        set_text(s_lat, "Latency    --");
    } else {
        snprintf(line, sizeof(line), "Latency    %d ms", r->latency_ms);
        set_text(s_lat, line);
    }
    fmt_mbps(line, sizeof(line), r->down_mbps);
    char down[72];
    snprintf(down, sizeof(down), "Download   %s", line);
    set_text(s_down, down);
    fmt_mbps(line, sizeof(line), r->up_mbps);
    char up[72];
    snprintf(up, sizeof(up), "Upload     %s", line);
    set_text(s_up, up);

    const char *prog = "";
    uint32_t col = COL_DIM;
    switch (r->phase) {
    case PHASE_LATENCY:
        prog = "Measuring latency...";
        col = COL_AQUA;
        break;
    case PHASE_DOWNLOAD:
        if (r->bytes_total) {
            snprintf(line, sizeof(line), "Downloading %u / %u KB",
                     (unsigned)(r->bytes_done / 1024), (unsigned)(r->bytes_total / 1024));
        } else {
            snprintf(line, sizeof(line), "Downloading %u KB...", (unsigned)(r->bytes_done / 1024));
        }
        prog = line;
        col = COL_AQUA;
        break;
    case PHASE_UPLOAD:
        if (r->bytes_total) {
            snprintf(line, sizeof(line), "Uploading %u / %u KB",
                     (unsigned)(r->bytes_done / 1024), (unsigned)(r->bytes_total / 1024));
        } else {
            snprintf(line, sizeof(line), "Uploading %u KB...", (unsigned)(r->bytes_done / 1024));
        }
        prog = line;
        col = COL_AQUA;
        break;
    case PHASE_DONE:
        prog = "Done. SELECT to run again.";
        col = COL_GREEN;
        break;
    case PHASE_FAIL:
        prog = r->err[0] ? r->err : "Failed";
        col = COL_ERR;
        break;
    default:
        prog = s_want_upload ? "SELECT run (down+up)  LEFT toggle up" : "SELECT run (down)  LEFT toggle up";
        col = COL_DIM;
        break;
    }
    set_text(s_prog, prog);
    lv_obj_set_style_text_color(s_prog, ui_col(col), 0);
}

static void publish_live(const result_t *r)
{
    s_live = *r;
}

static void apply_done(void *arg)
{
    result_t *r = arg;
    s_busy = false;
    if (!s_root) {
        free(r);
        return;
    }
    s_last = *r;
    s_live = *r;
    render_results(&s_last);
    render_link();
    if (s_hint) {
        lv_label_set_text(s_hint, "SELECT run  LEFT upload on/off  CANCEL exit");
    }
    free(r);
}

// ---- HTTP (badge only) ----------------------------------------------------

#ifndef SOLANA_OS_EMULATOR

static float mbps_of(size_t bytes, int64_t us)
{
    if (us <= 0 || bytes == 0) {
        return 0;
    }
    // bits / second / 1e6
    return (float)((bytes * 8.0) / ((double)us / 1e6) / 1e6);
}

static esp_err_t measure_latency(const char *url, int *out_ms)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(c, HTTP_METHOD_GET);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    int64_t clen = esp_http_client_fetch_headers(c);
    int64_t t1 = esp_timer_get_time();
    (void)clen;
    int status = esp_http_client_get_status_code(c);
    // Drain any body (bytes=0 should be empty).
    char sink[256];
    while (esp_http_client_read(c, sink, sizeof(sink)) > 0) {
        if (s_cancel) {
            break;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status / 100 != 2 && status != 0) {
        return ESP_FAIL;
    }
    *out_ms = (int)((t1 - t0) / 1000);
    if (*out_ms < 0) {
        *out_ms = 0;
    }
    return ESP_OK;
}

static esp_err_t measure_download(const char *url, size_t expect, result_t *r)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = CHUNK,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(c, HTTP_METHOD_GET);
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status / 100 != 2) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    size_t total = clen > 0 ? (size_t)clen : expect;
    r->bytes_total = total;
    r->bytes_done = 0;
    publish_live(r);

    char *buf = malloc(CHUNK);
    if (!buf) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    int64_t t0 = esp_timer_get_time();
    size_t got = 0;
    while (!s_cancel) {
        int n = esp_http_client_read(c, buf, CHUNK);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        got += (size_t)n;
        r->bytes_done = got;
        if ((got & 0x3fff) == 0) {       // ~every 16 KB
            publish_live(r);
        }
    }
    int64_t t1 = esp_timer_get_time();
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (s_cancel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK || got == 0) {
        return err != ESP_OK ? err : ESP_FAIL;
    }
    r->down_mbps = mbps_of(got, t1 - t0);
    r->bytes_done = got;
    return ESP_OK;
}

static esp_err_t measure_upload(const char *url, size_t bytes, result_t *r)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .buffer_size = CHUNK,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    esp_err_t err = esp_http_client_open(c, (int)bytes);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    char *buf = malloc(CHUNK);
    if (!buf) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0x5a, CHUNK);
    r->bytes_total = bytes;
    r->bytes_done = 0;
    publish_live(r);
    int64_t t0 = esp_timer_get_time();
    size_t sent = 0;
    while (sent < bytes && !s_cancel) {
        size_t n = bytes - sent;
        if (n > CHUNK) {
            n = CHUNK;
        }
        int w = esp_http_client_write(c, buf, (int)n);
        if (w <= 0) {
            err = ESP_FAIL;
            break;
        }
        sent += (size_t)w;
        r->bytes_done = sent;
        if ((sent & 0x3fff) == 0) {
            publish_live(r);
        }
    }
    int64_t t1 = esp_timer_get_time();
    free(buf);
    if (!s_cancel && err == ESP_OK) {
        (void)esp_http_client_fetch_headers(c);
        char sink[256];
        while (esp_http_client_read(c, sink, sizeof(sink)) > 0) {
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (s_cancel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK || sent == 0) {
        return err != ESP_OK ? err : ESP_FAIL;
    }
    r->up_mbps = mbps_of(sent, t1 - t0);
    r->bytes_done = sent;
    return ESP_OK;
}

static void worker(void *arg)
{
    (void)arg;
    result_t *r = calloc(1, sizeof(*r));
    if (!r) {
        s_busy = false;
        os_toast("Out of memory");
        vTaskDelete(NULL);
        return;
    }
    r->latency_ms = -1;
    r->down_mbps = -1;
    r->up_mbps = -1;
    r->do_upload = s_want_upload && using_cloudflare();

    char down_url[URL_MAX + 32];
    build_down_url(down_url, sizeof(down_url));

    // Free the badge-server TLS slot for our own client.
    os_http_hangup();

    r->phase = PHASE_LATENCY;
    publish_live(r);
    const char *lat_url = using_cloudflare() ? DEFAULT_LAT_URL : down_url;
    esp_err_t err = measure_latency(lat_url, &r->latency_ms);
    if (s_cancel) {
        snprintf(r->err, sizeof(r->err), "Cancelled");
        r->phase = PHASE_FAIL;
        os_post(apply_done, r);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "latency: %s", esp_err_to_name(err));
        // Continue: latency is nice-to-have; download is the point.
        r->latency_ms = -1;
    }

    r->phase = PHASE_DOWNLOAD;
    r->bytes_done = 0;
    r->bytes_total = s_url[0] ? 0 : s_dl_bytes;
    publish_live(r);
    err = measure_download(down_url, s_dl_bytes, r);
    if (s_cancel) {
        snprintf(r->err, sizeof(r->err), "Cancelled");
        r->phase = PHASE_FAIL;
        os_post(apply_done, r);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        snprintf(r->err, sizeof(r->err), err == ESP_ERR_NO_MEM ? "Need heap for TLS" : "Download failed");
        r->phase = PHASE_FAIL;
        os_post(apply_done, r);
        vTaskDelete(NULL);
        return;
    }

    if (r->do_upload) {
        r->phase = PHASE_UPLOAD;
        r->bytes_done = 0;
        r->bytes_total = UPLOAD_BYTES;
        publish_live(r);
        err = measure_upload(DEFAULT_UP_URL, UPLOAD_BYTES, r);
        if (s_cancel) {
            snprintf(r->err, sizeof(r->err), "Cancelled");
            r->phase = PHASE_FAIL;
            os_post(apply_done, r);
            vTaskDelete(NULL);
            return;
        }
        if (err != ESP_OK) {
            // Keep download result; mark upload missing.
            ESP_LOGW(TAG, "upload: %s", esp_err_to_name(err));
            r->up_mbps = -1;
            snprintf(r->err, sizeof(r->err), "Upload failed (down OK)");
            r->phase = PHASE_DONE;
            os_post(apply_done, r);
            vTaskDelete(NULL);
            return;
        }
    }

    r->phase = PHASE_DONE;
    r->err[0] = 0;
    os_post(apply_done, r);
    vTaskDelete(NULL);
}

#else  // SOLANA_OS_EMULATOR

static void worker(void *arg)
{
    (void)arg;
    result_t *r = calloc(1, sizeof(*r));
    if (!r) {
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    r->latency_ms = -1;
    r->down_mbps = -1;
    r->up_mbps = -1;
    snprintf(r->err, sizeof(r->err), "Run on the badge (not emulator)");
    r->phase = PHASE_FAIL;
    os_post(apply_done, r);
    vTaskDelete(NULL);
}

#endif

static void start_test(void)
{
    if (s_busy) {
        os_toast("Busy...");
        return;
    }
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    if (w.state != OS_WIFI_CONNECTED) {
        os_toast("Join Wi-Fi first");
        return;
    }
    load_config();
    s_cancel = false;
    s_busy = true;
    memset(&s_live, 0, sizeof(s_live));
    s_live.latency_ms = -1;
    s_live.down_mbps = -1;
    s_live.up_mbps = -1;
    s_live.phase = PHASE_LATENCY;
    render_results(&s_live);
    if (s_hint) {
        lv_label_set_text(s_hint, "SELECT cancel  CANCEL exit");
    }
    if (xTaskCreate(worker, "wifispeed", 8192, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        os_toast("Out of memory");
        render_results(&s_last);
    }
}

// ---- console --------------------------------------------------------------

static int cmd_wifispeed(int argc, char **argv)
{
    load_config();
    if (argc < 2 || !strcmp(argv[1], "status")) {
        char url[URL_MAX + 32], host[HOST_MAX];
        build_down_url(url, sizeof(url));
        host_of(url, host, sizeof(host));
        printf("url     %s\n", s_url[0] ? s_url : "(default Cloudflare __down)");
        printf("get     %s\n", url);
        printf("host    %s\n", host);
        printf("bytes   %u\n", (unsigned)s_dl_bytes);
        printf("upload  %s\n", s_want_upload ? "on" : "off");
        if (s_last.phase == PHASE_DONE || s_last.phase == PHASE_FAIL) {
            printf("last    lat=%d ms  down=%.2f Mbps  up=%.2f Mbps  %s\n",
                   s_last.latency_ms, (double)s_last.down_mbps, (double)s_last.up_mbps,
                   s_last.err[0] ? s_last.err : "ok");
        }
        return 0;
    }
    if (!strcmp(argv[1], "url") && argc >= 3) {
        if (!strcmp(argv[2], "clear") || !strcmp(argv[2], "default")) {
            nvs_set("url", "");
            s_url[0] = 0;
            printf("url cleared (Cloudflare default)\n");
            return 0;
        }
        if (strncmp(argv[2], "http://", 7) && strncmp(argv[2], "https://", 8)) {
            printf("url must start with http:// or https://\n");
            return 1;
        }
        if (!nvs_set("url", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_url, argv[2], sizeof(s_url));
        printf("url saved\n");
        return 0;
    }
    if (!strcmp(argv[1], "bytes") && argc >= 3) {
        unsigned long b = strtoul(argv[2], NULL, 10);
        if (b < 10000 || b > 8 * 1000 * 1000) {
            printf("bytes: 10000 .. 8000000\n");
            return 1;
        }
        if (!cfg_set_u32("bytes", (uint32_t)b)) {
            printf("nvs write failed\n");
            return 1;
        }
        s_dl_bytes = (uint32_t)b;
        printf("bytes %lu\n", b);
        return 0;
    }
    if (!strcmp(argv[1], "upload") && argc >= 3) {
        bool on = !strcmp(argv[2], "on") || !strcmp(argv[2], "1");
        bool off = !strcmp(argv[2], "off") || !strcmp(argv[2], "0");
        if (!on && !off) {
            printf("upload on|off\n");
            return 1;
        }
        if (!cfg_set_u32("upload", on ? 1 : 0)) {
            printf("nvs write failed\n");
            return 1;
        }
        s_want_upload = on;
        printf("upload %s\n", on ? "on" : "off");
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
        s_dl_bytes = DEFAULT_BYTES;
        s_want_upload = true;
        printf("cleared NVS wifispeed config (SD file untouched)\n");
        return 0;
    }
    printf("wifispeed [status|url <http(s)://...|clear>|bytes <n>|upload on|off|clear]\n");
    return 1;
}

static void wifispeed_register_cmd(void)
{
    static bool once;
    if (once) {
        return;
    }
    once = true;
    os_cmd_register("wifispeed",
                    "wifispeed [status|url <url|clear>|bytes <n>|upload on|off|clear] - Wi-Fi speed check",
                    cmd_wifispeed);
}

__attribute__((constructor)) static void wifispeed_ctor(void)
{
    wifispeed_register_cmd();
}

// ---- app lifecycle --------------------------------------------------------

static lv_obj_t *row_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *l = ui_label(parent, font, color, "");
    lv_obj_set_pos(l, 10, y);
    lv_obj_set_width(l, OS_ROOT_W - 20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_busy = false;
    s_cancel = false;
    load_config();
    memset(&s_last, 0, sizeof(s_last));
    s_last.latency_ms = -1;
    s_last.down_mbps = -1;
    s_last.up_mbps = -1;
    s_live = s_last;

    s_ssid = row_label(root, &sol_font_14, COL_TEXT, 8);
    s_rssi = row_label(root, &sol_mono_12, COL_DIM, 28);
    s_host = row_label(root, &sol_mono_12, COL_DIM, 46);

    s_lat = row_label(root, &sol_font_14, COL_TEXT, 78);
    s_down = row_label(root, &sol_font_20, COL_GREEN, 104);
    s_up = row_label(root, &sol_font_20, COL_AQUA, 132);
    s_prog = row_label(root, &sol_mono_12, COL_DIM, 168);
    lv_label_set_long_mode(s_prog, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(s_prog, 36);

    // Shared offline panel over the content; force-hidden once Wi-Fi has an IP
    // so a speed test is not blocked waiting for badge-server registration.
    s_offline = ui_offline_create(root, 0, CONTENT_H, false);
    s_hint = ui_hint(root, "SELECT run  LEFT upload on/off  CANCEL exit");
    render_link();
    render_results(&s_last);
}

static void leave(void)
{
    s_cancel = true;
    s_root = s_ssid = s_rssi = s_host = s_lat = s_down = s_up = s_prog = s_offline = s_hint = NULL;
}

static bool wifi_ready(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED;
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
    if (s_busy) {
        render_results(&s_live);
    } else {
        static int n;
        if (++n % 5 == 0) {
            render_link();
        }
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
        if (s_busy) {
            s_cancel = true;
            os_toast("Cancelling...");
            return true;
        }
        start_test();
        return true;
    }
    if (k == OS_KEY_LEFT && !repeat && !s_busy) {
        s_want_upload = !s_want_upload;
        cfg_set_u32("upload", s_want_upload ? 1 : 0);
        os_toast("Upload %s", s_want_upload ? "on" : "off");
        render_link();
        render_results(&s_last);
        return true;
    }
    return false;
}

const os_app_t app_wifispeed = {
    .id = "wifispeed",
    .name = "Wi-Fi Speed",
    .symbol = LV_SYMBOL_WIFI,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
