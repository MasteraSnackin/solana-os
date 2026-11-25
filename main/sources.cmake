# Source lists shared by the badge build (main/CMakeLists.txt) and the browser
# emulator (firmware/emulator/). Add new apps and portable core files to
# SOLANA_OS_SRCS so both builds pick them up; only code that talks to real
# hardware or ESP-IDF services belongs in SOLANA_OS_DEVICE_SRCS, and each of
# those needs an emulator counterpart.
#
# Bare BP26: Lua host/vendor and non-shell apps are left in-tree but not built.
# Core still calls lua_host_pause / lua_install_busy; lua/lua_stubs.c covers them.

set(SOLANA_OS_SRCS
    core/os.c core/launcher.c core/calibrate.c core/ui.c core/os_cloud.c core/os_settings.c core/os_wifi_boot.c
    core/os_mesh.c core/os_bridge.c core/os_api.c core/os_socket.c core/os_data.c core/os_cmd.c core/os_ota.c
    core/ed25519.c
    game/game.c game/xdex.c game/trade.c game/quests.c game/beacon.c
    apps/registry.c
    apps/wifi/wifi_app.c
    apps/badge/badge_app.c
    apps/settings/settings_app.c
    apps/update/update_app.c
    apps/about/about_app.c
    lua/lua_stubs.c
    assets/sol_mono_12.c assets/sol_font_14.c assets/sol_font_20.c assets/sol_font_32.c
    assets/img_logomark_16.c assets/img_logomark_40.c assets/img_logomark_72.c
    assets/img_foundation.c assets/img_foundation_black.c
)

set(SOLANA_OS_DEVICE_SRCS
    app_main.c
    core/os_wifi.c
    core/os_http.c
    core/os_ws.c
    core/os_ota_flash.c
    core/os_radio.c
    core/os_ble.c
    core/console.c
)
