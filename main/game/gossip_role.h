#pragma once

// Gossip roles (Solana OS 2.4.0; the parent repo's docs/GOSSIP.md §4 "Role
// HELLO" and "APP messages", §8). Builder: fw-roles. A badge whose owner
// turned on "May become a leader" (OS_SET_LEADER) can be offered a role by
// the server; in a session it broadcasts a second identity as a station
// (os_mesh_station + os_mesh_station_hello) with the role key's tags, answers
// claims with acks, runs the boss's metronome. Every badge also speaks the
// reserved app id `gossip`'s APP messages (claim, ack, bid, vote, boss, cure,
// cured) through this service, never through an app.
//
// It rides on the node service's sync (game/gossip.h): it never calls the
// server itself. The node service asks it for its part of each sync body
// (gossip_role_sync_fill) and hands it each answer (gossip_role_sync_done).
//
// Threads: OS task unless marked "any task".
//
// SEAM (foundation commit): the declarations between the "seam" lines are
// called by game/gossip.c (builder fw-node), the Settings app and
// lua/lua_gossip*.c. Their signatures are fixed; fill in the bodies in
// gossip_role.c. Add anything else below the seam.

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "gossip.h"

// ---- seam ------------------------------------------------------------------

// game_init() (after gossip_init) and the game tick (OS task).
void gossip_role_init(void);
void gossip_role_tick(int64_t now);
// Settings > May become a leader changed OS_SET_LEADER (OS task).
void gossip_role_setting_changed(void);

// Called by the node service (OS task) while building a sync body: add this
// service's fields to `body` (GOSSIP.md §7 GossipSync: `lead`, `role`,
// `heard`, `claims`, `cures`), within `room` bytes of printed JSON. Whatever
// it adds is kept until gossip_role_sync_done says it was delivered.
void gossip_role_sync_fill(cJSON *body, int room);
// Called by the node service (OS task) when that sync ended: delivered = the
// server answered 200 (the fields went: drop them), else they go again with
// the next. role = the answer's `role` (RoleState, GOSSIP.md §8), NULL when
// the answer had none or there was no answer. The cJSON is the caller's.
void gossip_role_sync_done(bool delivered, const cJSON *role);
// The node service's sync interval: true while a role session, an offer or a
// boss fight this badge joined wants the 2 s sync (GOSSIP.md §5). OS task.
bool gossip_role_fast_sync(void);
// The owner opted in to roles (OS_SET_LEADER): the node service syncs every
// 20 s. Any task.
bool gossip_role_opted_in(void);
// The node service heard a HELLO with a `g` field (OS task, after its own
// handling): `heard` roles and the boss radar.
void gossip_role_on_hello(const gossip_hello_t *h);

// ---- end of seam -----------------------------------------------------------
