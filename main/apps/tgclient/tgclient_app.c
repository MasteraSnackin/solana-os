// Telegram Bot API thin client for BP26 (ESP32-C5, 320×240).
//
// NOT Telegram Desktop / NOT user-account MTProto. The badge talks HTTPS to
// api.telegram.org (or an optional bridge host) with a BotFather token. It
// can only see updates the bot receives and send into a configured chat id.
//
// Spirit matches Hermes: UI + HTTP only, secrets in NVS/SD never in source.
// Lua `http` is unsuitable (badge-server proxy, size/time/host limits).

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

static const char *TAG = "tgclient";

#define NVS_NS          "tgclient"
#define TOKEN_FILE      BSP_SD_MOUNT "/tg.token"
#define CHAT_FILE       BSP_SD_MOUNT "/tg.chat"
#define API_FILE        BSP_SD_MOUNT "/tg.api"

#define API_DEFAULT     "https://api.telegram.org"
#define API_MAX         128
#define TOKEN_MAX       64
#define CHAT_MAX        32
#define MSG_MAX         200             // on-screen keyboard line
#define TG_LINE_MAX     240
#define LOG_MAX         3500
#define RESP_CAP        8192
#define BODY_CAP        512
#define URL_CAP         256
#define FETCH_LIMIT     15

#define CONTENT_H       (OS_ROOT_H - UI_HINT_H)
#define SCROLL_STEP     40
#define POLL_TICKS      150             // ~15 s at OS_TICK_MS=100

typedef enum {
    JOB_NONE = 0,
    JOB_POLL,
    JOB_SEND,
    JOB_DISCOVER,                       // getUpdates → pick a chat id for the wizard
} job_t;

typedef enum {
    WIZ_NONE = 0,
    WIZ_TOKEN,                          // type BotFather token on-device
    WIZ_CHAT,                           // type chat id / @user (or discover)
} wiz_t;

static lv_obj_t *s_root, *s_status, *s_scroll, *s_body, *s_offline, *s_hint;
static ui_kb_t *s_kb;
static bool s_busy;
static job_t s_job;
static wiz_t s_wiz;
static char s_log[LOG_MAX];
static int s_poll_ticks;

static char s_api[API_MAX];
static char s_token[TOKEN_MAX];
static char s_chat[CHAT_MAX];
static char s_draft[MSG_MAX];
static int64_t s_offset;                // next getUpdates offset (update_id+1)

static void open_wizard(wiz_t step);
static void update_hint(void);
static bool config_ready(void);
static bool start_job(job_t job);

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

static bool tg_nvs_get_i64(const char *key, int64_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_i64(h, key, out);
    nvs_close(h);
    return err == ESP_OK;
}

static bool tg_nvs_set_i64(const char *key, int64_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_i64(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static void normalize_api(char *api, size_t cap)
{
    trim_inplace(api);
    if (!api[0]) {
        strlcpy(api, API_DEFAULT, cap);
        return;
    }
    size_t n = strlen(api);
    while (n > 0 && api[n - 1] == '/') {
        api[--n] = 0;
    }
    // Allow "api.telegram.org" without scheme.
    if (!strstr(api, "://")) {
        char tmp[API_MAX];
        snprintf(tmp, sizeof(tmp), "https://%s", api);
        strlcpy(api, tmp, cap);
    }
}

static void load_config(void)
{
    s_api[0] = s_token[0] = s_chat[0] = 0;
    if (!nvs_get("api", s_api, sizeof(s_api))) {
        if (!read_file_line(API_FILE, s_api, sizeof(s_api))) {
            strlcpy(s_api, API_DEFAULT, sizeof(s_api));
        }
    }
    normalize_api(s_api, sizeof(s_api));
    if (!nvs_get("token", s_token, sizeof(s_token))) {
        read_file_line(TOKEN_FILE, s_token, sizeof(s_token));
    }
    if (!nvs_get("chat", s_chat, sizeof(s_chat))) {
        read_file_line(CHAT_FILE, s_chat, sizeof(s_chat));
    }
    if (!tg_nvs_get_i64("offset", &s_offset)) {
        s_offset = 0;
    }
}

static bool wifi_ready(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED;
}

static bool config_ready(void)
{
    return s_token[0] && s_chat[0];
}

static void update_hint(void)
{
    if (!s_hint) {
        return;
    }
    if (!s_token[0]) {
        lv_label_set_text(s_hint, "SELECT setup token  LEFT reset");
    } else if (!s_chat[0]) {
        lv_label_set_text(s_hint, "SELECT set chat  RIGHT find chat  LEFT reset");
    } else {
        lv_label_set_text(s_hint, "SELECT type  RIGHT refresh  UP/DOWN scroll  LEFT clear/setup");
    }
}

// Build https://host/bot<token>/<method>[?query]. Never log the result.
static bool bot_url(char *out, size_t cap, const char *method, const char *query)
{
    if (!s_api[0] || !s_token[0] || !method || !method[0]) {
        return false;
    }
    if (query && query[0]) {
        return snprintf(out, cap, "%s/bot%s/%s?%s", s_api, s_token, method, query) < (int)cap;
    }
    return snprintf(out, cap, "%s/bot%s/%s", s_api, s_token, method) < (int)cap;
}

// ---- transcript -----------------------------------------------------------

static void render_log(void)
{
    if (!s_body) {
        return;
    }
    if (s_log[0]) {
        lv_label_set_text(s_body, s_log);
    } else if (!s_token[0]) {
        lv_label_set_text(s_body,
                          "Telegram Bot API (not full Telegram).\n\n"
                          "SELECT → enter bot token\n"
                          "(from @BotFather).\n\n"
                          "Or UART: tg token …\n"
                          "Or /sdcard/tg.token");
    } else if (!s_chat[0]) {
        lv_label_set_text(s_body,
                          "Token saved.\n\n"
                          "SELECT → type chat id\n"
                          "RIGHT → find chat from\n"
                          "recent bot messages.\n\n"
                          "Message the bot first,\n"
                          "then find or type the id.");
    } else {
        lv_label_set_text(s_body,
                          "Ready. SELECT to type.\n"
                          "RIGHT refresh. LEFT clears\n"
                          "the log (empty LEFT = setup).");
    }
    lv_obj_update_layout(s_scroll);
    lv_coord_t bottom = lv_obj_get_scroll_bottom(s_scroll);
    if (bottom > 0) {
        lv_obj_scroll_by_bounded(s_scroll, 0, bottom, LV_ANIM_OFF);
    }
}

static void append_log(const char *who, const char *text)
{
    char chunk[TG_LINE_MAX];
    snprintf(chunk, sizeof(chunk), "%s: %s\n\n", who, text);
    size_t have = strlen(s_log), need = strlen(chunk);
    if (have + need >= sizeof(s_log)) {
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
        const char *msg = LV_SYMBOL_REFRESH "  fetching...";
        if (s_job == JOB_SEND) {
            msg = LV_SYMBOL_REFRESH "  sending...";
        } else if (s_job == JOB_DISCOVER) {
            msg = LV_SYMBOL_REFRESH "  finding chat...";
        }
        lv_label_set_text(s_status, msg);
        lv_obj_set_style_text_color(s_status, ui_col(COL_YELLOW), 0);
        return;
    }
    if (!wifi_ready()) {
        lv_label_set_text(s_status, LV_SYMBOL_WIFI "  join Wi-Fi first");
        lv_obj_set_style_text_color(s_status, ui_col(COL_ORANGE), 0);
        return;
    }
    if (!s_token[0]) {
        lv_label_set_text(s_status, "SELECT → enter bot token");
        lv_obj_set_style_text_color(s_status, ui_col(COL_ORANGE), 0);
        return;
    }
    if (!s_chat[0]) {
        lv_label_set_text(s_status, "SELECT → chat id   RIGHT find");
        lv_obj_set_style_text_color(s_status, ui_col(COL_ORANGE), 0);
        return;
    }
    char host[40] = "";
    const char *p = strstr(s_api, "://");
    p = p ? p + 3 : s_api;
    snprintf(host, sizeof(host), "%.*s", (int)strcspn(p, "/:?"), p);
    bool bridge = strcmp(s_api, API_DEFAULT) != 0;
    lv_label_set_text_fmt(s_status, LV_SYMBOL_OK "  %s  ·  chat %s%s",
                          host, s_chat, bridge ? "  · bridge" : "");
    lv_obj_set_style_text_color(s_status, ui_col(COL_GREEN), 0);
}

// ---- HTTP worker ----------------------------------------------------------

typedef struct {
    char *added;                        // heap: new transcript lines, or NULL
    char discovered[CHAT_MAX];          // JOB_DISCOVER chat id, or empty
    char err[96];
    bool sent_ok;
} result_t;

static void apply_result(void *arg)
{
    result_t *r = arg;
    job_t done = s_job;
    s_busy = false;
    s_job = JOB_NONE;
    if (!s_root) {
        free(r->added);
        free(r);
        return;
    }
    if (done == JOB_DISCOVER) {
        if (r->discovered[0]) {
            if (nvs_set("chat", r->discovered)) {
                strlcpy(s_chat, r->discovered, sizeof(s_chat));
                os_toast("Chat %s", s_chat);
                render_log();
                render_status(NULL);
                update_hint();
                if (wifi_ready() && config_ready()) {
                    // Fall through into a normal poll after discover.
                    free(r->added);
                    free(r);
                    start_job(JOB_POLL);
                    return;
                }
            } else {
                os_toast("nvs write failed");
            }
        } else if (r->err[0]) {
            render_status(r->err);
            os_toast("%s", r->err);
        } else {
            os_toast("No chats yet — message the bot");
            render_status(NULL);
            open_wizard(WIZ_CHAT);
            free(r->added);
            free(r);
            return;
        }
        free(r->added);
        free(r);
        update_hint();
        return;
    }
    if (r->added && r->added[0]) {
        size_t have = strlen(s_log), need = strlen(r->added);
        if (have + need >= sizeof(s_log)) {
            size_t drop = have + need + 1 - sizeof(s_log);
            if (drop >= have) {
                s_log[0] = 0;
            } else {
                memmove(s_log, s_log + drop, have - drop + 1);
            }
        }
        strlcat(s_log, r->added, sizeof(s_log));
        render_log();
        render_status(NULL);
    } else if (r->sent_ok) {
        render_status(NULL);
    } else if (r->err[0]) {
        render_status(r->err);
        os_toast("%s", r->err);
    } else {
        render_status(NULL);
    }
    free(r->added);
    free(r);
    update_hint();
}

static bool chat_matches(const cJSON *chat)
{
    if (!s_chat[0] || !chat) {
        return false;
    }
    cJSON *id = cJSON_GetObjectItem(chat, "id");
    if (cJSON_IsNumber(id)) {
        char buf[CHAT_MAX];
        snprintf(buf, sizeof(buf), "%.0f", id->valuedouble);
        return !strcmp(buf, s_chat);
    }
    if (cJSON_IsString(id) && id->valuestring) {
        return !strcmp(id->valuestring, s_chat);
    }
    const char *uname = cJSON_GetStringValue(cJSON_GetObjectItem(chat, "username"));
    if (uname && uname[0]) {
        if (s_chat[0] == '@' && !strcmp(uname, s_chat + 1)) {
            return true;
        }
        if (!strcmp(uname, s_chat)) {
            return true;
        }
    }
    return false;
}

static const char *sender_name(const cJSON *msg)
{
    const cJSON *from = cJSON_GetObjectItem(msg, "from");
    const char *name = from ? cJSON_GetStringValue(cJSON_GetObjectItem(from, "first_name")) : NULL;
    if (name && name[0]) {
        return name;
    }
    name = from ? cJSON_GetStringValue(cJSON_GetObjectItem(from, "username")) : NULL;
    if (name && name[0]) {
        return name;
    }
    return "Chat";
}

// Parse getUpdates JSON; append matching messages into *lines (heap). Advance offset.
static bool parse_updates(const char *json, char **lines_out, char *err, size_t err_cap)
{
    *lines_out = NULL;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, err_cap, "bad JSON");
        return false;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"))) {
        const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "description"));
        snprintf(err, err_cap, "%s", desc && desc[0] ? desc : "API not ok");
        cJSON_Delete(root);
        return false;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return true;
    }

    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        snprintf(err, err_cap, "Out of memory");
        cJSON_Delete(root);
        return false;
    }
    buf[0] = 0;
    int64_t max_id = s_offset > 0 ? s_offset - 1 : 0;
    bool any = false;

    cJSON *upd;
    cJSON_ArrayForEach(upd, result) {
        cJSON *uid = cJSON_GetObjectItem(upd, "update_id");
        if (cJSON_IsNumber(uid) && (int64_t)uid->valuedouble > max_id) {
            max_id = (int64_t)uid->valuedouble;
        }
        cJSON *msg = cJSON_GetObjectItem(upd, "message");
        if (!msg) {
            msg = cJSON_GetObjectItem(upd, "edited_message");
        }
        if (!msg || !chat_matches(cJSON_GetObjectItem(msg, "chat"))) {
            continue;
        }
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
        if (!text || !text[0]) {
            // Non-text (sticker, photo, …): one-line stub.
            text = "(non-text message)";
        }
        char line[TG_LINE_MAX];
        snprintf(line, sizeof(line), "%s: %s\n\n", sender_name(msg), text);
        size_t need = strlen(line);
        if (len + need + 1 > cap) {
            size_t ncap = cap * 2;
            while (len + need + 1 > ncap) {
                ncap *= 2;
            }
            char *nbuf = realloc(buf, ncap);
            if (!nbuf) {
                break;
            }
            buf = nbuf;
            cap = ncap;
        }
        memcpy(buf + len, line, need + 1);
        len += need;
        any = true;
    }

    if (max_id > 0 && max_id + 1 > s_offset) {
        s_offset = max_id + 1;
        tg_nvs_set_i64("offset", s_offset);
    }
    cJSON_Delete(root);
    if (!any) {
        free(buf);
        *lines_out = NULL;
        return true;
    }
    *lines_out = buf;
    return true;
}

// Pick the newest chat id from getUpdates (wizard "find chat"). Does not advance offset.
static bool parse_discover_chat(const char *json, char *out, size_t out_cap, char *err, size_t err_cap)
{
    out[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, err_cap, "bad JSON");
        return false;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"))) {
        const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "description"));
        snprintf(err, err_cap, "%s", desc && desc[0] ? desc : "API not ok");
        cJSON_Delete(root);
        return false;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return true;
    }
    int64_t best_uid = -1;
    char best[CHAT_MAX] = "";
    cJSON *upd;
    cJSON_ArrayForEach(upd, result) {
        cJSON *uid = cJSON_GetObjectItem(upd, "update_id");
        int64_t idn = cJSON_IsNumber(uid) ? (int64_t)uid->valuedouble : -1;
        cJSON *msg = cJSON_GetObjectItem(upd, "message");
        if (!msg) {
            msg = cJSON_GetObjectItem(upd, "edited_message");
        }
        if (!msg) {
            continue;
        }
        cJSON *chat = cJSON_GetObjectItem(msg, "chat");
        cJSON *cid = chat ? cJSON_GetObjectItem(chat, "id") : NULL;
        char buf[CHAT_MAX] = "";
        if (cJSON_IsNumber(cid)) {
            snprintf(buf, sizeof(buf), "%.0f", cid->valuedouble);
        } else if (cJSON_IsString(cid) && cid->valuestring) {
            strlcpy(buf, cid->valuestring, sizeof(buf));
        }
        if (!buf[0]) {
            continue;
        }
        if (idn >= best_uid) {
            best_uid = idn;
            strlcpy(best, buf, sizeof(best));
        }
    }
    cJSON_Delete(root);
    if (best[0]) {
        strlcpy(out, best, out_cap);
    }
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    job_t job = s_job;
    result_t *r = calloc(1, sizeof(*r));
    if (!r) {
        s_busy = false;
        s_job = JOB_NONE;
        os_toast("Out of memory");
        vTaskDelete(NULL);
        return;
    }

    load_config();
    if (!s_token[0]) {
        snprintf(r->err, sizeof(r->err), "token missing");
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }
    if (job != JOB_DISCOVER && !s_chat[0]) {
        snprintf(r->err, sizeof(r->err), "chat missing");
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    char url[URL_CAP];
    char *resp = malloc(RESP_CAP);
    if (!resp) {
        snprintf(r->err, sizeof(r->err), "Out of memory");
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    // One TLS connection at a time: release badge-server keep-alive.
    os_http_hangup();

    if (job == JOB_SEND) {
        char body[BODY_CAP];
        cJSON *root = cJSON_CreateObject();
        // chat_id may be numeric string or @username — Telegram accepts both as JSON string.
        cJSON_AddStringToObject(root, "chat_id", s_chat);
        cJSON_AddStringToObject(root, "text", s_draft);
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            snprintf(r->err, sizeof(r->err), "Out of memory");
            free(resp);
            os_post(apply_result, r);
            vTaskDelete(NULL);
            return;
        }
        if (strlen(json) + 1 > sizeof(body)) {
            snprintf(r->err, sizeof(r->err), "message too long");
            cJSON_free(json);
            free(resp);
            os_post(apply_result, r);
            vTaskDelete(NULL);
            return;
        }
        strlcpy(body, json, sizeof(body));
        cJSON_free(json);

        if (!bot_url(url, sizeof(url), "sendMessage", NULL)) {
            snprintf(r->err, sizeof(r->err), "bad API URL");
            free(resp);
            os_post(apply_result, r);
            vTaskDelete(NULL);
            return;
        }

        int status = 0;
        esp_err_t err = os_http_request("POST", url, NULL, body, resp, RESP_CAP, &status);
        // Scrub token-bearing URL from this frame as best-effort.
        memset(url, 0, sizeof(url));
        if (err != ESP_OK) {
            snprintf(r->err, sizeof(r->err), err == ESP_ERR_NO_MEM ? "need heap for TLS" : "HTTP failed");
            free(resp);
            os_post(apply_result, r);
            vTaskDelete(NULL);
            return;
        }
        if (status / 100 != 2) {
            cJSON *root2 = cJSON_Parse(resp);
            const char *desc = root2 ? cJSON_GetStringValue(cJSON_GetObjectItem(root2, "description")) : NULL;
            snprintf(r->err, sizeof(r->err), "%s", desc && desc[0] ? desc : "send failed");
            cJSON_Delete(root2);
            free(resp);
            os_post(apply_result, r);
            vTaskDelete(NULL);
            return;
        }
        free(resp);
        r->sent_ok = true;
        ESP_LOGI(TAG, "sendMessage ok");
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    // JOB_POLL or JOB_DISCOVER — both use getUpdates.
    char query[64];
    // Discover needs the backlog: offset=0 so old messages still reveal a chat.
    int64_t off = (job == JOB_DISCOVER) ? 0 : s_offset;
    snprintf(query, sizeof(query), "offset=%lld&limit=%d&timeout=0",
             (long long)off, FETCH_LIMIT);
    if (!bot_url(url, sizeof(url), "getUpdates", query)) {
        snprintf(r->err, sizeof(r->err), "bad API URL");
        free(resp);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    int status = 0;
    esp_err_t err = os_http_request("GET", url, NULL, NULL, resp, RESP_CAP, &status);
    memset(url, 0, sizeof(url));
    if (err != ESP_OK) {
        snprintf(r->err, sizeof(r->err), err == ESP_ERR_NO_MEM ? "need heap for TLS" : "HTTP failed");
        free(resp);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }
    if (status / 100 != 2) {
        snprintf(r->err, sizeof(r->err), "HTTP %d", status);
        free(resp);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    if (job == JOB_DISCOVER) {
        if (!parse_discover_chat(resp, r->discovered, sizeof(r->discovered), r->err, sizeof(r->err))) {
            free(resp);
            os_post(apply_result, r);
            vTaskDelete(NULL);
            return;
        }
        free(resp);
        ESP_LOGI(TAG, "discover chat %s", r->discovered[0] ? "(found)" : "(none)");
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }

    if (!parse_updates(resp, &r->added, r->err, sizeof(r->err))) {
        free(resp);
        os_post(apply_result, r);
        vTaskDelete(NULL);
        return;
    }
    free(resp);
    ESP_LOGI(TAG, "getUpdates ok (offset now %lld)", (long long)s_offset);
    os_post(apply_result, r);
    vTaskDelete(NULL);
}

static bool start_job(job_t job)
{
    if (s_busy) {
        os_toast("Busy...");
        return false;
    }
    load_config();
    if (!s_token[0]) {
        os_toast("Set bot token first");
        render_status(NULL);
        return false;
    }
    if (job != JOB_DISCOVER && !s_chat[0]) {
        os_toast("Set chat id first");
        render_status(NULL);
        return false;
    }
    if (!wifi_ready()) {
        os_toast("Wi-Fi is off");
        render_status(NULL);
        return false;
    }
    s_busy = true;
    s_job = job;
    s_poll_ticks = 0;
    render_status(NULL);
    if (xTaskCreate(worker, "tg_http", 8192, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        s_job = JOB_NONE;
        os_toast("Out of memory");
        render_status(NULL);
        return false;
    }
    return true;
}

static void send_message(const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    strlcpy(s_draft, text, sizeof(s_draft));
    trim_inplace(s_draft);
    if (!s_draft[0]) {
        return;
    }
    append_log("You", s_draft);
    if (!start_job(JOB_SEND)) {
        // Keep the optimistic "You:" line; status already shows why.
    }
}

static void open_wizard(wiz_t step)
{
    if (!s_root || s_kb || s_busy) {
        return;
    }
    if (step == WIZ_NONE) {
        s_wiz = WIZ_NONE;
        return;
    }
    if (step == WIZ_TOKEN) {
        s_wiz = WIZ_TOKEN;
        s_kb = ui_kb_create(s_root, "Bot token (BotFather)", TOKEN_MAX - 1);
        ui_kb_set_secret(s_kb, true);
        return;
    }
    s_wiz = WIZ_CHAT;
    s_kb = ui_kb_create(s_root, "Chat id or @username", CHAT_MAX - 1);
    if (s_chat[0]) {
        ui_kb_set_text(s_kb, s_chat);
    }
}

static void wizard_finish(ui_kb_result_t r)
{
    wiz_t step = s_wiz;
    char typed[TOKEN_MAX];
    typed[0] = 0;
    if (r == UI_KB_DONE) {
        strlcpy(typed, ui_kb_text(s_kb), sizeof(typed));
        trim_inplace(typed);
    }
    ui_kb_delete(s_kb);
    s_kb = NULL;
    s_wiz = WIZ_NONE;

    if (r != UI_KB_DONE || !typed[0]) {
        render_log();
        render_status(NULL);
        update_hint();
        return;
    }

    if (step == WIZ_TOKEN) {
        if (strlen(typed) >= TOKEN_MAX) {
            os_toast("token too long");
            open_wizard(WIZ_TOKEN);
            return;
        }
        if (!nvs_set("token", typed)) {
            os_toast("nvs write failed");
            render_status(NULL);
            update_hint();
            return;
        }
        strlcpy(s_token, typed, sizeof(s_token));
        memset(typed, 0, sizeof(typed));
        os_toast("Token saved");
        render_log();
        render_status(NULL);
        update_hint();
        if (!s_chat[0]) {
            open_wizard(WIZ_CHAT);
        }
        return;
    }

    if (step == WIZ_CHAT) {
        if (!nvs_set("chat", typed)) {
            os_toast("nvs write failed");
            render_status(NULL);
            update_hint();
            return;
        }
        strlcpy(s_chat, typed, sizeof(s_chat));
        os_toast("Chat saved");
        render_log();
        render_status(NULL);
        update_hint();
        if (wifi_ready() && config_ready()) {
            start_job(JOB_POLL);
        }
    }
}

static void reset_setup(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "token");
        nvs_erase_key(h, "chat");
        nvs_erase_key(h, "offset");
        nvs_commit(h);
        nvs_close(h);
    }
    s_token[0] = s_chat[0] = 0;
    s_offset = 0;
    s_log[0] = 0;
    os_toast("Setup reset");
    render_log();
    render_status(NULL);
    update_hint();
    open_wizard(WIZ_TOKEN);
}

// ---- console --------------------------------------------------------------

static int cmd_tg(int argc, char **argv)
{
    load_config();
    if (argc < 2 || !strcmp(argv[1], "status")) {
        char host[48] = "";
        const char *p = strstr(s_api, "://");
        p = p ? p + 3 : s_api;
        snprintf(host, sizeof(host), "%.*s", (int)strcspn(p, "/:?"), p);
        printf("api    %s\n", s_api);
        printf("host   %s%s\n", host, strcmp(s_api, API_DEFAULT) ? " (bridge/custom)" : "");
        printf("token  %s\n", s_token[0] ? "(set)" : "(missing — NVS or /sdcard/tg.token)");
        printf("chat   %s\n", s_chat[0] ? s_chat : "(unset — NVS or /sdcard/tg.chat)");
        printf("offset %lld\n", (long long)s_offset);
        printf("note   Bot API only — not user MTProto / Telegram Desktop\n");
        return 0;
    }
    if (!strcmp(argv[1], "token") && argc >= 3) {
        if (strlen(argv[2]) >= TOKEN_MAX) {
            printf("token too long (max %d)\n", TOKEN_MAX - 1);
            return 1;
        }
        if (!nvs_set("token", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_token, argv[2], sizeof(s_token));
        printf("token saved (not printed)\n");
        return 0;
    }
    if (!strcmp(argv[1], "chat") && argc >= 3) {
        if (!nvs_set("chat", argv[2])) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_chat, argv[2], sizeof(s_chat));
        printf("chat saved\n");
        return 0;
    }
    if (!strcmp(argv[1], "api") && argc >= 3) {
        char buf[API_MAX];
        strlcpy(buf, argv[2], sizeof(buf));
        normalize_api(buf, sizeof(buf));
        if (!nvs_set("api", buf)) {
            printf("nvs write failed\n");
            return 1;
        }
        strlcpy(s_api, buf, sizeof(s_api));
        printf("api saved %s\n", s_api);
        return 0;
    }
    if (!strcmp(argv[1], "refresh") || !strcmp(argv[1], "poll")) {
        if (!start_job(JOB_POLL)) {
            return 1;
        }
        printf("polling getUpdates…\n");
        return 0;
    }
    if (!strcmp(argv[1], "send") && argc >= 3) {
        char buf[MSG_MAX];
        buf[0] = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2) {
                strlcat(buf, " ", sizeof(buf));
            }
            strlcat(buf, argv[i], sizeof(buf));
        }
        send_message(buf);
        return 0;
    }
    if (!strcmp(argv[1], "clear")) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        s_token[0] = s_chat[0] = 0;
        s_offset = 0;
        strlcpy(s_api, API_DEFAULT, sizeof(s_api));
        printf("cleared NVS tgclient config (SD files untouched)\n");
        return 0;
    }
    printf("tg [status|token <secret>|chat <id>|api <base>|refresh|send <text>|clear]\n");
    return 1;
}

static void tg_register_cmd(void)
{
    static bool once;
    if (once) {
        return;
    }
    once = true;
    os_cmd_register("tg",
                    "tg [status|token <secret>|chat <id>|api <base>|refresh|send <text>|clear] - Telegram Bot API thin client",
                    cmd_tg);
}

__attribute__((constructor)) static void tg_ctor(void)
{
    tg_register_cmd();
}

// ---- app lifecycle --------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_kb = NULL;
    s_busy = false;
    s_job = JOB_NONE;
    s_wiz = WIZ_NONE;
    s_poll_ticks = 0;
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
    s_hint = ui_hint(root, "");
    update_hint();

    if (!config_ready()) {
        // On-device wizard — no UART required for first-time setup.
        open_wizard(!s_token[0] ? WIZ_TOKEN : WIZ_CHAT);
    } else if (wifi_ready()) {
        start_job(JOB_POLL);
    }
}

static void leave(void)
{
    if (s_kb) {
        ui_kb_delete(s_kb);
        s_kb = NULL;
    }
    s_wiz = WIZ_NONE;
    s_root = s_status = s_scroll = s_body = s_offline = s_hint = NULL;
}

static bool key(os_key_t k, bool repeat)
{
    if (s_kb) {
        ui_kb_result_t r = ui_kb_key(s_kb, k, repeat);
        if (r != UI_KB_BUSY) {
            if (s_wiz != WIZ_NONE) {
                wizard_finish(r);
            } else {
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
        }
        return true;
    }
    if (ui_offline_key(s_offline, k)) {
        return true;
    }
    if (s_offline && !lv_obj_is_hidden(s_offline) && !wifi_ready()) {
        return k != OS_KEY_BACK;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        if (s_busy) {
            os_toast("Busy...");
            return true;
        }
        if (!s_token[0]) {
            open_wizard(WIZ_TOKEN);
            return true;
        }
        if (!s_chat[0]) {
            open_wizard(WIZ_CHAT);
            return true;
        }
        s_wiz = WIZ_NONE;
        s_kb = ui_kb_create(s_root, "Message", sizeof(s_draft) - 1);
        return true;
    }
    if (k == OS_KEY_RIGHT && !repeat) {
        if (!s_token[0]) {
            os_toast("Set bot token first");
            open_wizard(WIZ_TOKEN);
            return true;
        }
        if (!s_chat[0]) {
            // Find chat from recent bot updates (message the bot first).
            start_job(JOB_DISCOVER);
            return true;
        }
        start_job(JOB_POLL);
        return true;
    }
    if ((k == OS_KEY_UP || k == OS_KEY_DOWN) && s_scroll) {
        lv_obj_scroll_by_bounded(s_scroll, 0, k == OS_KEY_UP ? SCROLL_STEP : -SCROLL_STEP, LV_ANIM_ON);
        return true;
    }
    if (k == OS_KEY_LEFT && !repeat) {
        if (s_log[0]) {
            s_log[0] = 0;
            render_log();
            os_toast("Cleared");
            update_hint();
        } else {
            // Empty log + LEFT → re-run on-device setup wizard.
            reset_setup();
        }
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
    // Need Wi-Fi IP only — not badge-server hello (same idea as Browser).
    if (wifi_ready() && s_offline) {
        lv_obj_set_hidden(s_offline, true);
    }
    if (!s_busy && ++s_poll_ticks >= POLL_TICKS) {
        s_poll_ticks = 0;
        if (wifi_ready() && config_ready()) {
            start_job(JOB_POLL);
        } else {
            render_status(NULL);
        }
    } else if (!s_busy && (s_poll_ticks % 5) == 0) {
        render_status(NULL);
    }
}

const os_app_t app_tgclient = {
    .id = "tgclient",
    .name = "Telegram",
    .symbol = LV_SYMBOL_BELL,
    .accent = COL_BLUE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
