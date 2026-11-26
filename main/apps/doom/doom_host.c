// Host around vendored doomgeneric: boot, tick, soft-fail, shutdown.

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "doomgeneric.h"
#include "doom_host.h"
#include "doomgeneric_solana.h"
#include "core/os_mem.h"

static const char *TAG = "doom";

static jmp_buf s_jmp;
static volatile bool s_in_engine;
static volatile bool s_alive;
static volatile bool s_fatal;
static char s_title[64];
static uint16_t *s_rgb;
static char s_wad[200];

void doom_host_fatal(const char *msg)
{
    ESP_LOGE(TAG, "fatal: %s", msg ? msg : "(null)");
    s_fatal = true;
    s_alive = false;
    if (s_in_engine) {
        longjmp(s_jmp, 1);
    }
}

bool doom_core_boot(const char *wad_path)
{
    if (!wad_path || !wad_path[0]) {
        return false;
    }
    doom_core_halt();
    doom_host_reset_error();
    s_fatal = false;
    s_alive = false;
    strlcpy(s_wad, wad_path, sizeof(s_wad));
    const char *slash = strrchr(wad_path, '/');
    strlcpy(s_title, slash ? slash + 1 : wad_path, sizeof(s_title));

    s_rgb = heap_caps_malloc((size_t)DOOM_WIDTH * DOOM_HEIGHT * sizeof(uint16_t),
                             os_mem_bulk_caps());
    if (!s_rgb) {
        ESP_LOGE(TAG, "no PSRAM for 320x200 RGB565");
        return false;
    }
    memset(s_rgb, 0, (size_t)DOOM_WIDTH * DOOM_HEIGHT * sizeof(uint16_t));
    doom_solana_set_fb(s_rgb);
    doom_solana_reset_input();

    static char arg0[] = "doom";
    static char arg_iwad[] = "-iwad";
    static char arg_nosound[] = "-nosound";
    static char arg_nomusic[] = "-nomusic";
    static char arg_nogui[] = "-nogui";
    static char arg_mb[] = "-mb";
    static char arg_mb_v[] = "6";
    // Must outlive Create: myargv keeps this pointer for demos / M_CheckParm.
    static char path[200];
    static char *argv[] = {
        arg0, arg_iwad, path, arg_nosound, arg_nomusic, arg_nogui, arg_mb, arg_mb_v,
    };
    strlcpy(path, wad_path, sizeof(path));
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));

    s_in_engine = true;
    if (setjmp(s_jmp) != 0) {
        s_in_engine = false;
        ESP_LOGE(TAG, "boot aborted");
        doom_core_halt();
        return false;
    }

    ESP_LOGI(TAG, "create %s", path);
    doomgeneric_Create(argc, argv);
    s_in_engine = false;

    if (s_fatal) {
        doom_core_halt();
        return false;
    }
    s_alive = true;
    ESP_LOGI(TAG, "running %s", s_title);
    return true;
}

void doom_core_halt(void)
{
    s_alive = false;
    if (DG_ScreenBuffer || s_rgb) {
        // Safe even if Create never finished: Shutdown nulls what it can.
        doomgeneric_Shutdown();
    }
    doom_solana_set_fb(NULL);
    heap_caps_free(s_rgb);
    s_rgb = NULL;
    doom_solana_reset_input();
    s_fatal = false;
}

int doom_core_tick(uint32_t joy, const uint16_t **fb)
{
    if (!s_alive) {
        return -1;
    }
    if (fb) {
        *fb = NULL;
    }
    doom_solana_feed_joy(joy);

    s_in_engine = true;
    if (setjmp(s_jmp) != 0) {
        s_in_engine = false;
        s_alive = false;
        return -1;
    }
    doomgeneric_Tick();
    s_in_engine = false;

    if (s_fatal) {
        s_alive = false;
        return -1;
    }
    if (doom_solana_take_frame()) {
        if (fb) {
            *fb = s_rgb;
        }
        return 1;
    }
    return 0;
}

bool doom_core_alive(void)
{
    return s_alive;
}

const char *doom_core_title(void)
{
    return s_title;
}
