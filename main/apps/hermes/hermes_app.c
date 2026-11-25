// Thin Wi-Fi Hermes / OpenAI-compatible chat client.
//
// The badge is UI + HTTP only: models run on a remote OpenAI-compatible
// endpoint (Nous Portal, OpenRouter, local Hermes API server, vLLM, …).
// Lua `http` cannot do this well (badge-server proxy, 2 KB body / ~3.5 KB
// answer / 6 s / host allowlist), so this is a native app that POSTs
// /v1/chat/completions over the badge's Wi-Fi with os_http.
//
// Secrets never live in source. Load API key from NVS (console `hermes key`)
// or /sdcard/hermes.key. Base URL and model from NVS or matching SD files.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_cmd.h"
#include "core/os_http.h"
#include "core/os_wifi.h"
#include "core/ui.h"

static const char *TAG = "hermes";

#define NVS_NS          "hermes"
#define KEY_FILE        BSP_SD_MOUNT "/hermes.key"
#define URL_FILE        BSP_SD_MOUNT "/hermes.url"
#define MODEL_FILE      BSP_SD_MOUNT "/hermes.model"

#define URL_MAX         192
#define KEY_MAX         160
#define MODEL_MAX       64
#define MSG_MAX         200             // one typed line (on-screen keyboard)
#define TURN_MAX        8               // kept user/assistant pairs (truncated)
#define TURN_TEXT_MAX   480
#define LOG_MAX         3500
#define RESP_CAP        6144
#define BODY_CAP        6144

#define CONTENT_H       (OS_ROOT_H - UI_HINT_H)

typedef struct {
    char role;                          // 'u' or 'a'
    char text[TURN_TEXT_MAX];
} turn_t;

static lv_obj_t *s_root, *s_status, *s_scroll, *s_body, *s_offline, *s_hint;
static ui_kb_t *s_kb;
static bool s_busy;
static char s_log[LOG_MAX];
static turn_t s_turns[TURN_MAX];
static int s_n_turns;

static char s_url[URL_MAX];
static char s_key[KEY_MAX];
static char s_model[MODEL_MAX];
static char s_draft[MSG_MAX];

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
    // strip a trailing newline from SD files
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

static void load_config(void)
{
    s_url[0] = s_key[0] = s_model[0] = 0;
    if (!nvs_get("url", s_url, sizeof(s_url))) {
        read_file_line(URL_FILE, s_url, sizeof(s_url));
    }
    if (!nvs_get("key", s_key, sizeof(s_key))) {
        read_file_line(KEY_FILE, s_key, sizeof(s_key));
    }
    if (!nvs_get("model", s_model, sizeof(s_model))) {
        if (!read_file_line(MODEL_FILE, s_model, sizeof(s_model))) {
            strlcpy(s_model, "hermes-agent", sizeof(s_model));
        }
    }
}

static void completions_url(char *out, size_t cap)
{
    char base[URL_MAX];
    strlcpy(base, s_url, sizeof(base));
    trim_inplace(base);
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        base[--n] = 0;
    }
    if (strstr(base, "/chat/completions")) {
        strlcpy(out, base, cap);
        return;
    }
    if (n >= 3 && !strcmp(base + n - 3, "/v1")) {
        snprintf(out, cap, "%s/chat/completions", base);
    } else {
        snprintf(out, cap, "%s/v1/chat/completions", base);
    }
}

// ---- transcript -----------------------------------------------------------

static void render_log(void)
{
    if (!s_body) {
        return;
    }
    lv_label_set_text(s_body, s_log[0] ? s_log : "SELECT to type a message.\n"
                      "Config: console `hermes`, or SD files\n"
                      "/sdcard/hermes.url + hermes.key\n"
                      "(+ optional hermes.model).");
    lv_obj_update_layout(s_scroll);
    lv_coord_t bottom = lv_obj_get_scroll_bottom(s_scroll);
    if (bottom > 0) {
        lv_obj_scroll_by_bounded(s_scroll, 0, bottom, LV_ANIM_OFF);
    }
}

static void append_log(const char *who, const char *text)
{
    char chunk[TURN_TEXT_MAX + 24];
    snprintf(chunk, sizeof(chunk), "%s: %s\n\n", who, text);
    size_t have = strlen(s_log), need = strlen(chunk);
    if (have + need >= sizeof(s_log)) {
        // Drop from the front until it fits.
        size_t drop = have + need + 1 - sizeof(s_log);
        if (drop >= have) {
            s_log[0] = 0;
        } else {
            memmove(s_log, s_log + drop, have - drop + 1);
        }
    }
    strlcat(s_log, chunk, sizeof(s_log));
    render_log();
}

static void push_turn(char role, const char *text)
{
    if (s_n_turns == TURN_MAX) {
        memmove(&s_turns[0], &s_turns[1], sizeof(s_turns[0]) * (TURN_MAX - 1));
        s_n_turns--;
    }
    turn_t *t = &s_turns[s_n_turns++];
    t->role = role;
    strlcpy(t->text, text, sizeof(t->text));
}

static void render_status(const char *override_msg)
{
    if (!s_status) {
        return;
    }
    if (override_msg) {
        lv_label_set_text(s_status, override_msg);
        lv_obj_set_style_text_color(s_status, ui_col(COL_YELLOW), 0);
        return;
    }
    if (s_busy) {
        lv_label_set_text(s_status, LV_SYMBOL_REFRESH "  waiting for reply...");
        lv_obj_set_style_text_color(s_status, ui_col(COL_YELLOW), 0);
        return;
    }
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    if (w.state != OS_WIFI_CONNECTED) {
        lv_label_set_text(s_status, LV_SYMBOL_WIFI "  join Wi-Fi first");
        lv_obj_set_style_text_color(s_status, ui_col(COL_ORANGE), 0);
        return;
    }
    if (!s_url[0]) {
        lv_label_set_text(s_status, "set base URL (hermes url …)");
        lv_obj_set_style_text_color(s_status, ui_col(COL_ORANGE), 0);
        return;
    }
    if (!s_key[0]) {
        lv_label_set_text(s_status, "set API key (hermes key … / SD)");
        lv_obj_set_style_text_color(s_status, ui_col(COL_ORANGE), 0);
        return;
    }
    char host[48] = "";
    const char *p = strstr(s_url, "://");
    p = p ? p + 3 : s_url;
    snprintf(host, sizeof(host), "%.*s", (int)strcspn(p, "/:?"), p);
    lv_label_set_text_fmt(s_status, LV_SYMBOL_OK "  %s  ·  %s", host, s_model);
    lv_obj_set_style_text_color(s_status, ui_col(COL_GREEN), 0);
}

// ---- HTTP worker ----------------------------------------------------------

typedef struct {
    char *reply;                        // heap, freed on UI
    char err[96];
} result_t;

static void apply_result(void *arg)
{
    result_t *r = arg;
    s_busy = false;
    if (!s_root) {
        free(r->reply);
        free(r);
        return;
    }
    if (r->reply && r->reply[0]) {
        push_turn('a', r->reply);
        append_log("Hermes", r->reply);
        render_status(NULL);
    } else {
        render_status(r->err[0] ? r->err : "request failed");
        os_toast("%s", r->err[0] ? r->err : "request failed");
    }
    free(r->reply);
    free(r);
    if (s_hint) {
        lv_label_set_text(s_hint, "SELECT type  UP/DOWN scroll  LEFT clear");
    }
}

static char *build_body(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON_AddStringToObject(root, "model", s_model);
    cJSON_AddBoolToObject(root, "stream", false);
    // Keep prompts short so the badge's HTTP body stays manageable.
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", "You are Hermes. Reply briefly; this client is a small badge screen.");
    cJSON_AddItemToArray(msgs, sys);
    for (int i = 0; i < s_n_turns; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", s_turns[i].role == 'u' ? "user" : "assistant");
        cJSON_AddStringToObject(m, "content", s_turns[i].text);
        cJSON_AddItemToArray(msgs, m);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static bool parse_reply(const char *json, char *out, size_t cap, char *err, size_t err_cap)
{
    out[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, err_cap, "bad JSON");
        return false;
    }
    cJSON *e = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(e)) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
        snprintf(err, err_cap, "%s", msg && msg[0] ? msg : "API error");
        cJSON_Delete(root);
        return false;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = first ? cJSON_GetObjectItem(first, "message") : NULL;
    const char *content = message ? cJSON_GetStringValue(cJSON_GetObjectItem(message, "content")) : NULL;
    if (!content) {
        // Some gateways put text on the choice itself.
        content = first ? cJSON_GetStringValue(cJSON_GetObjectItem(first, "text")) : NULL;
    }
    if (!content || !content[0]) {
        snprintf(err, err_cap, "empty reply");
        cJSON_Delete(root);
        return false;
    }
    strlcpy(out, content, cap);
    cJSON_Delete(root);
    return true;
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

    char url[URL_MAX + 32];
    completions_url(url, sizeof(url));
    char *body = build_body();
    if (!body) {
        snprintf(r->err, sizeof(r->err), "Out of memory");
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }
    if (strlen(body) + 1 > BODY_CAP) {
        snprintf(r->err, sizeof(r->err), "prompt too large");
        cJSON_free(body);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    char *resp = malloc(RESP_CAP);
    if (!resp) {
        snprintf(r->err, sizeof(r->err), "Out of memory");
        cJSON_free(body);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    // One TLS connection at a time: release the badge-server keep-alive so
    // we can open the configured OpenAI-compatible host.
    os_http_hangup();

    int status = 0;
    const char *bearer = s_key[0] ? s_key : NULL;
    esp_err_t err = os_http_request("POST", url, bearer, body, resp, RESP_CAP, &status);
    cJSON_free(body);
    // Scrub the key from this task's stack frames that may have held auth.
    (void)bearer;

    if (err != ESP_OK) {
        snprintf(r->err, sizeof(r->err), err == ESP_ERR_NO_MEM ? "need heap for TLS" : "HTTP failed");
        free(resp);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }
    if (status / 100 != 2) {
        // Prefer API error text when present.
        char tmp[TURN_TEXT_MAX];
        if (parse_reply(resp, tmp, sizeof(tmp), r->err, sizeof(r->err)) || !r->err[0]) {
            snprintf(r->err, sizeof(r->err), "HTTP %d", status);
        }
        free(resp);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    char parsed[TURN_TEXT_MAX];
    if (!parse_reply(resp, parsed, sizeof(parsed), r->err, sizeof(r->err))) {
        free(resp);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }
    free(resp);
    size_t n = strlen(parsed) + 1;
    r->reply = malloc(n);
    if (r->reply) {
        memcpy(r->reply, parsed, n);
    } else {
        snprintf(r->err, sizeof(r->err), "Out of memory");
    }
    os_post(apply_result, r);
    vTaskDelete(NULL);
}

static void send_message(const char *text)
{
    if (s_busy) {
        os_toast("Busy...");
        return;
    }
    load_config();
    if (!s_url[0]) {
        os_toast("Set hermes URL first");
        render_status(NULL);
        return;
    }
    if (!s_key[0]) {
        os_toast("Set hermes key first");
        render_status(NULL);
        return;
    }
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    if (w.state != OS_WIFI_CONNECTED) {
        os_toast("Wi-Fi is off");
        render_status(NULL);
        return;
    }

    push_turn('u', text);
    append_log("You", text);
    s_busy = true;
    render_status(NULL);
    if (xTaskCreate(worker, "hermes_http", 8192, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        os_toast("Out of memory");
        render_status(NULL);
    }
}

// ---- console --------------------------------------------------------------

static int cmd_hermes(int argc, char **argv)
{
    load_config();
    if (argc < 2 || !strcmp(argv[1], "status")) {
        char url[URL_MAX + 32];
        if (s_url[0]) {
            completions_url(url, sizeof(url));
        } else {
            strlcpy(url, "(unset)", sizeof(url));
        }
        printf("url    %s\n", s_url[0] ? s_url : "(unset — NVS or /sdcard/hermes.url)");
        printf("post   %s\n", url);
        printf("model  %s\n", s_model);
        printf("key    %s\n", s_key[0] ? "(set)" : "(missing — NVS or /sdcard/hermes.key)");
        return 0;
    }
    if (!strcmp(argv[1], "url") && argc >= 3) {
        if (!nvs_set("url", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_url, argv[2], sizeof(s_url));
        printf("url saved\n");
        return 0;
    }
    if (!strcmp(argv[1], "key") && argc >= 3) {
        // os_http Authorization buffer is 160 bytes including "Bearer ".
        if (strlen(argv[2]) > 150) {
            printf("key too long (max 150)\n");
            return 1;
        }
        if (!nvs_set("key", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_key, argv[2], sizeof(s_key));
        printf("key saved (not printed)\n");
        return 0;
    }
    if (!strcmp(argv[1], "model") && argc >= 3) {
        if (!nvs_set("model", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_model, argv[2], sizeof(s_model));
        printf("model saved\n");
        return 0;
    }
    if (!strcmp(argv[1], "clear")) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        s_url[0] = s_key[0] = 0;
        strlcpy(s_model, "hermes-agent", sizeof(s_model));
        printf("cleared NVS hermes config (SD files untouched)\n");
        return 0;
    }
    printf("hermes [status|url <base>|key <secret>|model <id>|clear]\n");
    return 1;
}

static void hermes_register_cmd(void)
{
    static bool once;
    if (once) {
        return;
    }
    once = true;
    os_cmd_register("hermes",
                    "hermes [status|url <base>|key <secret>|model <id>|clear] - OpenAI-compatible chat client",
                    cmd_hermes);
}

__attribute__((constructor)) static void hermes_ctor(void)
{
    hermes_register_cmd();
}

// ---- app lifecycle --------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_kb = NULL;
    s_busy = false;
    load_config();

    s_status = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_status, 6, 4);
    lv_obj_set_width(s_status, OS_ROOT_W - 12);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);

    s_scroll = lv_obj_create(root);
    lv_obj_set_pos(s_scroll, 0, 22);
    lv_obj_set_size(s_scroll, OS_ROOT_W, CONTENT_H - 22);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_scroll, 6, 0);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    s_body = ui_label(s_scroll, &sol_mono_12, COL_TEXT, "");
    lv_obj_set_width(s_body, OS_ROOT_W - 16);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_MODE_WRAP);

    s_offline = ui_offline_create(root, 22, CONTENT_H - 22, false);
    render_log();
    render_status(NULL);
    s_hint = ui_hint(root, "SELECT type  UP/DOWN scroll  LEFT clear");
}

static void leave(void)
{
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    s_root = s_status = s_scroll = s_body = s_offline = s_hint = NULL;
    // Worker may still finish; apply_result no-ops without s_root.
}

static bool key(os_key_t k, bool repeat)
{
    if (s_kb) {
        ui_kb_result_t r = ui_kb_key(s_kb, k, repeat);
        if (r != UI_KB_BUSY) {
            if (r == UI_KB_DONE) {
                strlcpy(s_draft, ui_kb_text(s_kb), sizeof(s_draft));
                trim_inplace(s_draft);
            }
            ui_kb_delete(s_kb);
            s_kb = NULL;
            if (r == UI_KB_DONE && s_draft[0]) {
                send_message(s_draft);
                s_draft[0] = 0;
            }
        }
        return true;
    }
    if (ui_offline_key(s_offline, k)) {
        return true;
    }
    if (s_offline && !lv_obj_is_hidden(s_offline)) {
        return k != OS_KEY_BACK;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        if (s_busy) {
            os_toast("Busy...");
            return true;
        }
        s_kb = ui_kb_create(s_root, "Message Hermes", sizeof(s_draft) - 1);
        return true;
    }
    if ((k == OS_KEY_UP || k == OS_KEY_DOWN) && s_scroll) {
        lv_obj_scroll_by_bounded(s_scroll, 0, k == OS_KEY_UP ? 40 : -40, LV_ANIM_ON);
        return true;
    }
    if (k == OS_KEY_LEFT && !repeat) {
        s_n_turns = 0;
        s_log[0] = 0;
        render_log();
        os_toast("Cleared");
        return true;
    }
    return false;
}

static void tick(void)
{
    if (!s_root || s_kb) {
        return;
    }
    ui_offline_update(s_offline);
    static int n;
    if (!s_busy && ++n % 5 == 0) {
        render_status(NULL);
    }
}

const os_app_t app_hermes = {
    .id = "hermes",
    .name = "Hermes",
    .symbol = LV_SYMBOL_EDIT,
    .accent = COL_LPURPLE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
