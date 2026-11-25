// Host logic tests for Breakout power-ups / multiball.
// Build & run (no ESP-IDF):
//   cc -std=c11 -Wall -Wextra -O2 -o /tmp/test_breakout_logic \
//        main/apps/breakout/test_breakout_logic.c && /tmp/test_breakout_logic

#include <stdio.h>
#include <string.h>

#include "breakout_logic.h"

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_fail++;
    } else {
        printf("ok: %s\n", msg);
    }
}

static void test_pick_and_spawn_chance(void)
{
    expect(bo_pick_powerup(0) == BO_PU_STICKY, "rnd0 → sticky");
    expect(bo_pick_powerup(1) == BO_PU_MULTI, "rnd1 → multi");
    expect(bo_should_spawn_drop(0), "spawn when rnd % 4 == 0");
    expect(!bo_should_spawn_drop(1), "no spawn when rnd % 4 == 1");
    expect(bo_should_spawn_drop(4), "spawn when rnd % 4 == 0 (4)");
}

static void test_drop_paddle_hit(void)
{
    bo_drop_t d = { .alive = true, .kind = BO_PU_MULTI, .x = 100, .y = 150 };
    expect(bo_drop_hits_paddle(&d, 90, 48, 154, 6), "drop overlaps paddle");
    d.x = 10;
    expect(!bo_drop_hits_paddle(&d, 90, 48, 154, 6), "drop misses paddle");
    d.alive = false;
    d.x = 100;
    expect(!bo_drop_hits_paddle(&d, 90, 48, 154, 6), "dead drop never hits");
}

static void test_spawn_drop_slot(void)
{
    bo_drop_t drops[BO_MAX_DROPS];
    memset(drops, 0, sizeof(drops));
    int s0 = bo_spawn_drop(drops, BO_MAX_DROPS, BO_PU_MULTI, 160, 40);
    expect(s0 == 0, "first drop slot 0");
    expect(drops[0].alive && drops[0].kind == BO_PU_MULTI, "drop 0 multi alive");
    expect(drops[0].x == 160 - BO_DROP_W / 2, "drop centred on brick");
    for (int i = 1; i < BO_MAX_DROPS; i++) {
        expect(bo_spawn_drop(drops, BO_MAX_DROPS, BO_PU_STICKY, 10, 10) == i,
               "fill remaining drop slots");
    }
    expect(bo_spawn_drop(drops, BO_MAX_DROPS, BO_PU_MULTI, 0, 0) < 0,
           "no slot when full");
    expect(bo_spawn_drop(drops, BO_MAX_DROPS, BO_PU_NONE, 0, 0) < 0,
           "NONE kind rejected");
}

static void test_multiball(void)
{
    bo_ball_t balls[BO_MAX_BALLS];
    memset(balls, 0, sizeof(balls));
    balls[0].alive = true;
    balls[0].x = 100;
    balls[0].y = 80;
    balls[0].vx = 3;
    balls[0].vy = -3;

    int n = bo_activate_multiball(balls, BO_MAX_BALLS, 3);
    expect(n == 1, "one sibling from one ball");
    expect(bo_alive_balls(balls, BO_MAX_BALLS) == 2, "two balls alive");
    expect(balls[1].alive && balls[1].vx == -3, "sibling mirrors vx");

    n = bo_activate_multiball(balls, BO_MAX_BALLS, 3);
    expect(n == 1, "third ball from two (one free slot)");
    expect(bo_alive_balls(balls, BO_MAX_BALLS) == 3, "three balls max");

    n = bo_activate_multiball(balls, BO_MAX_BALLS, 3);
    expect(n == 0, "no spawn when full");
}

static void test_sticky_glue_launch(void)
{
    bo_ball_t b;
    memset(&b, 0, sizeof(b));
    bo_glue_ball(&b, 100, 48, 154, 5);
    expect(b.alive && b.stuck && b.vx == 0 && b.vy == 0, "glue sets stuck");
    expect(b.x == 100 + 48 / 2 - 5 / 2, "ball centred on paddle");
    expect(b.y == 154 - 5 - 1, "ball sits above paddle");

    bo_launch_ball(&b, 4, 1);
    expect(!b.stuck && b.vx == 4 && b.vy == -4, "launch clears stuck, goes up");

    bo_ball_t balls[BO_MAX_BALLS];
    memset(balls, 0, sizeof(balls));
    bo_glue_ball(&balls[0], 50, 48, 154, 5);
    bo_glue_ball(&balls[1], 50, 48, 154, 5);
    expect(bo_any_stuck(balls, BO_MAX_BALLS), "any_stuck true");
    int launched = bo_launch_all_stuck(balls, BO_MAX_BALLS, 3, 0);
    expect(launched == 2, "launch all stuck");
    expect(!bo_any_stuck(balls, BO_MAX_BALLS), "none stuck after launch");

    bo_glue_ball(&balls[0], 20, 48, 154, 5);
    bo_sync_stuck_balls(balls, BO_MAX_BALLS, 80, 48, 154, 5);
    expect(balls[0].x == 80 + 48 / 2 - 5 / 2, "sync moves stuck with paddle");
}

static void test_life_rule_alive_count(void)
{
    bo_ball_t balls[BO_MAX_BALLS];
    memset(balls, 0, sizeof(balls));
    expect(bo_alive_balls(balls, BO_MAX_BALLS) == 0, "empty → lose life");
    balls[0].alive = true;
    balls[2].alive = true;
    expect(bo_alive_balls(balls, BO_MAX_BALLS) == 2, "two alive");
    balls[0].alive = false;
    expect(bo_alive_balls(balls, BO_MAX_BALLS) == 1, "one remains — no life loss");
}

int main(void)
{
    test_pick_and_spawn_chance();
    test_drop_paddle_hit();
    test_spawn_drop_slot();
    test_multiball();
    test_sticky_glue_launch();
    test_life_rule_alive_count();

    if (g_fail) {
        fprintf(stderr, "\n%d assertion(s) failed\n", g_fail);
        return 1;
    }
    printf("\nAll breakout logic tests passed.\n");
    return 0;
}
