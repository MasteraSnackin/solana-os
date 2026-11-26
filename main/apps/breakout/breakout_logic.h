// Pure Breakout power-up / multiball helpers (no LVGL / ESP deps).
// Shared by breakout_app.c and the host logic test.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BO_MAX_BALLS    3
#define BO_MAX_DROPS    4
#define BO_DROP_SPEED   2
#define BO_DROP_W       12
#define BO_DROP_H       8
#define BO_SPAWN_CHANCE 4   /* 1 in N brick breaks spawn a drop */

typedef enum {
    BO_PU_NONE = 0,
    BO_PU_MULTI = 1,
    BO_PU_STICKY = 2,
} bo_powerup_t;

typedef struct {
    bool alive;
    int x, y;
    int vx, vy;
    bool stuck;
} bo_ball_t;

typedef struct {
    bool alive;
    bo_powerup_t kind;
    int x, y;
} bo_drop_t;

/* Pick power-up kind from a 32-bit random value. */
static inline bo_powerup_t bo_pick_powerup(uint32_t rnd)
{
    return (rnd & 1u) ? BO_PU_MULTI : BO_PU_STICKY;
}

/* True when this brick break should spawn a falling capsule. */
static inline bool bo_should_spawn_drop(uint32_t rnd)
{
    return (rnd % (uint32_t)BO_SPAWN_CHANCE) == 0;
}

static inline int bo_alive_balls(const bo_ball_t *balls, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (balls[i].alive) {
            c++;
        }
    }
    return c;
}

static inline int bo_free_ball_slot(const bo_ball_t *balls, int n)
{
    for (int i = 0; i < n; i++) {
        if (!balls[i].alive) {
            return i;
        }
    }
    return -1;
}

static inline int bo_free_drop_slot(const bo_drop_t *drops, int n)
{
    for (int i = 0; i < n; i++) {
        if (!drops[i].alive) {
            return i;
        }
    }
    return -1;
}

/* Spawn a falling capsule centred on (cx, cy). Returns slot or -1. */
static inline int bo_spawn_drop(bo_drop_t *drops, int n, bo_powerup_t kind, int cx, int cy)
{
    int slot = bo_free_drop_slot(drops, n);
    if (slot < 0 || kind == BO_PU_NONE) {
        return -1;
    }
    drops[slot].alive = true;
    drops[slot].kind = kind;
    drops[slot].x = cx - BO_DROP_W / 2;
    drops[slot].y = cy;
    return slot;
}

/* AABB: drop vs paddle. */
static inline bool bo_drop_hits_paddle(const bo_drop_t *d, int paddle_x, int paddle_w,
                                       int paddle_y, int paddle_h)
{
    if (!d->alive) {
        return false;
    }
    return d->x + BO_DROP_W > paddle_x &&
           d->x < paddle_x + paddle_w &&
           d->y + BO_DROP_H > paddle_y &&
           d->y < paddle_y + paddle_h;
}

/*
 * Multiball: from each alive free-flying ball, try to add one sibling with
 * mirrored horizontal velocity. Returns how many new balls were created.
 */
static inline int bo_activate_multiball(bo_ball_t *balls, int n, int speed)
{
    if (speed < 1) {
        speed = 1;
    }
    /* Snapshot which were alive before we mutate. */
    bool was[BO_MAX_BALLS];
    int alive_n = 0;
    for (int i = 0; i < n && i < BO_MAX_BALLS; i++) {
        was[i] = balls[i].alive && !balls[i].stuck;
        if (was[i]) {
            alive_n++;
        }
    }
    if (alive_n == 0) {
        /* If only stuck balls, still spawn from first alive. */
        for (int i = 0; i < n; i++) {
            if (balls[i].alive) {
                was[i] = true;
                break;
            }
        }
    }

    int spawned = 0;
    for (int i = 0; i < n && i < BO_MAX_BALLS; i++) {
        if (!was[i]) {
            continue;
        }
        int slot = bo_free_ball_slot(balls, n);
        if (slot < 0) {
            break;
        }
        balls[slot] = balls[i];
        balls[slot].alive = true;
        balls[slot].stuck = false;
        int hx = balls[i].vx;
        if (hx == 0) {
            hx = speed;
        }
        balls[slot].vx = -hx;
        if (balls[slot].vy == 0) {
            balls[slot].vy = -speed;
        }
        /* nudge so they separate */
        balls[slot].x += (balls[slot].vx > 0) ? 4 : -4;
        spawned++;
    }
    return spawned;
}

/* Glue ball i onto the paddle (sticky catch or life reset). */
static inline void bo_glue_ball(bo_ball_t *b, int paddle_x, int paddle_w,
                                int paddle_y, int ball_s)
{
    b->alive = true;
    b->stuck = true;
    b->vx = 0;
    b->vy = 0;
    b->x = paddle_x + paddle_w / 2 - ball_s / 2;
    b->y = paddle_y - ball_s - 1;
}

/* Launch a stuck ball upward with horizontal sign from rnd bit0. */
static inline void bo_launch_ball(bo_ball_t *b, int speed, uint32_t rnd)
{
    if (!b->alive || !b->stuck) {
        return;
    }
    b->stuck = false;
    b->vx = (rnd & 1u) ? speed : -speed;
    b->vy = -speed;
}

/* Launch every stuck ball (SELECT while sticky or ready). */
static inline int bo_launch_all_stuck(bo_ball_t *balls, int n, int speed, uint32_t rnd)
{
    int launched = 0;
    for (int i = 0; i < n; i++) {
        if (balls[i].alive && balls[i].stuck) {
            bo_launch_ball(&balls[i], speed, rnd ^ (uint32_t)(i * 3));
            launched++;
        }
    }
    return launched;
}

static inline bool bo_any_stuck(const bo_ball_t *balls, int n)
{
    for (int i = 0; i < n; i++) {
        if (balls[i].alive && balls[i].stuck) {
            return true;
        }
    }
    return false;
}

/* Keep stuck balls centred on the paddle as it moves. */
static inline void bo_sync_stuck_balls(bo_ball_t *balls, int n,
                                       int paddle_x, int paddle_w,
                                       int paddle_y, int ball_s)
{
    for (int i = 0; i < n; i++) {
        if (balls[i].alive && balls[i].stuck) {
            balls[i].x = paddle_x + paddle_w / 2 - ball_s / 2;
            balls[i].y = paddle_y - ball_s - 1;
        }
    }
}
