// gossip for Lua apps, the role side (docs/LUA_API.md "gossip", Solana OS
// 2.4.0; the parent repo's docs/GOSSIP.md §10): role, boss, the Lab's acting
// calls that speak to roles. Builder: fw-roles. Reads game/gossip_role.h.
//
// Foundation stub: adds nothing to the table.

#include "lua_priv.h"

// The `gossip` table is on top of the stack: add functions with
// lua_setfield(L, -2, name) and leave the stack as it was.
void lh_gossip_role_open(lua_State *L)
{
    (void)L;
}
