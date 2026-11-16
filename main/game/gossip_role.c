// Gossip roles (Solana OS 2.4.0, game/gossip_role.h). Builder: fw-roles.
//
// Foundation stub: compiles and does nothing. No role, no offer, nothing
// added to a sync, never the fast sync.

#include "gossip_role.h"
#include "core/os_settings.h"

void gossip_role_init(void)
{
}

void gossip_role_tick(int64_t now)
{
    (void)now;
}

void gossip_role_setting_changed(void)
{
}

void gossip_role_sync_fill(cJSON *body, int room)
{
    (void)body;
    (void)room;
}

void gossip_role_sync_done(bool delivered, const cJSON *role)
{
    (void)delivered;
    (void)role;
}

bool gossip_role_fast_sync(void)
{
    return false;
}

bool gossip_role_opted_in(void)
{
    return false;
}

void gossip_role_on_hello(const gossip_hello_t *h)
{
    (void)h;
}
