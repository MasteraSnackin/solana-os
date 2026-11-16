// gossip for Lua apps (docs/LUA_API.md "gossip", Solana OS 2.4.0; the parent
// repo's docs/GOSSIP.md §10): the node side, reading game/gossip.h's state.
// Builder: fw-node. lua_gossip_role.c (fw-roles) adds the role side to the
// same table (lh_gossip_role_open).
//
// Foundation: the table and the role side's hook are in place, and
// lua_host_gossip (on_gossip) works; the functions are the builders'.

#include <string.h>
#include "lua_host.h"
#include "lua_priv.h"

void lh_open_gossip(lua_State *L)
{
    static const luaL_Reg gossip[] = {
        {NULL, NULL},
    };
    luaL_newlib(L, gossip);
    lh_gossip_role_open(L);
    lua_setglobal(L, "gossip");
}

bool lua_host_gossip(const char *ev_json)
{
    return ev_json && lua_host_broadcast("on_gossip", ev_json, LUA_PERM_GOSSIP);
}
