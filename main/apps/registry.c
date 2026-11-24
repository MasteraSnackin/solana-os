// Every installed app, in launcher order. To add an app: create
// apps/<name>/<name>_app.c exporting an os_app_t, add it to SOLANA_OS_SRCS in
// main/sources.cmake (the badge and the emulator both build from it) and list
// it here.

#include "core/os.h"

extern const os_app_t app_wifi, app_badge, app_ledbadge, app_espnow, app_debug, app_settings, app_about;
extern const os_app_t app_guide, app_xdex, app_trade, app_quests, app_beacon, app_store, app_apps;
extern const os_app_t app_timer;
extern const os_app_t app_calc;
extern const os_app_t app_snake;
extern const os_app_t app_guide, app_xdex, app_trade, app_quests, app_beacon, app_store, app_apps, app_tetris;
extern const os_app_t app_guide, app_xdex, app_trade, app_quests, app_beacon, app_store, app_apps, app_gbemu;
extern const os_app_t app_wifi, app_scanner, app_paint, app_badge, app_ledbadge, app_espnow, app_debug, app_settings, app_about;
extern const os_app_t app_guide, app_xdex, app_trade, app_quests, app_beacon, app_store, app_apps, app_gbemu, app_gbaemu, app_snesemu;

const os_app_t *const os_apps[] = {
    &app_wifi,
    &app_scanner,
    &app_paint,
    &app_badge,
    &app_ledbadge,
    &app_guide,
    &app_xdex,
    &app_trade,
    &app_quests,
    &app_beacon,            // only on badges paired as a quest beacon (visible())
    &app_store,
    &app_apps,
    &app_timer,
    &app_calc,
    &app_snake,
    &app_tetris,
    &app_gbemu,
    &app_gbaemu,
    &app_snesemu,
    &app_espnow,
    &app_debug,
    &app_settings,
    &app_about,
};

const int os_app_count = sizeof(os_apps) / sizeof(os_apps[0]);
