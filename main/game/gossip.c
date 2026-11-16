// Gossip, the node service (Solana OS 2.4.0, game/gossip.h). Builder: fw-node.
//
// Foundation stub: compiles and does nothing. Gossip is off, there is no
// clock, no class, no Cfg beyond the defaults, and no action is queued.

#include <string.h>
#include "gossip.h"

void gossip_init(void)
{
}

void gossip_tick(int64_t now)
{
    (void)now;
}

void gossip_setting_changed(void)
{
}

bool gossip_on(void)
{
    return false;
}

int64_t gossip_server_ms(void)
{
    return 0;
}

char gossip_class(void)
{
    return 0;
}

void gossip_cfg(gossip_cfg_t *out)
{
    static const int rssi[4] = {-50, -60, -70, -80}, wt[4] = {10, 6, 3, 1};
    memset(out, 0, sizeof(*out));
    out->thr = 100;
    out->decay = 2;
    memcpy(out->wt_rssi, rssi, sizeof(rssi));
    memcpy(out->wt, wt, sizeof(wt));
    out->offpeak_from = out->offpeak_to = -1;
    out->close = -55;
}

bool gossip_held_spam(char out[GOSSIP_ADDR_MAX + 1])
{
    out[0] = 0;
    return false;
}

bool gossip_genome(const char *addr, gossip_genome_t *out)
{
    (void)addr;
    memset(out, 0, sizeof(*out));
    return false;
}

bool gossip_action(const char *json)
{
    (void)json;
    return false;
}

void gossip_sync_soon(void)
{
}
