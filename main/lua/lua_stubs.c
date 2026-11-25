// Bare-badge stubs: Lua host and store install are not built (see
// main/sources.cmake). Core and game still call a few of these symbols.

#include <stddef.h>
#include <string.h>
#include "lua/lua_host.h"
#include "lua/lua_install.h"

size_t lua_host_heap_max(void)
{
    return 0;
}

const os_app_t lua_host_app = {
    .id = "lua",
    .name = "Lua",
    .symbol = NULL,
    .icon = NULL,
    .accent = 0,
    .on_enter = NULL,
    .on_exit = NULL,
    .on_key = NULL,
    .on_tick = NULL,
};

bool lua_host_launch(const char *id)
{
    (void)id;
    return false;
}

const char *lua_host_running(void)
{
    return NULL;
}

bool lua_host_active(const char *id)
{
    (void)id;
    return false;
}

void lua_host_init(void) {}

void lua_host_pause(void) {}

void lua_host_unpause(void) {}

esp_err_t lua_install_start(const char *id, const char *name)
{
    (void)id;
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

void lua_install_get(lua_install_info_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->state = LUA_INSTALL_IDLE;
    }
}

bool lua_install_busy(const char *id)
{
    (void)id;
    return false;
}

void lua_install_recover(void) {}

void lua_install_init(void) {}
