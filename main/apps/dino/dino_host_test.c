/*
 * Host unit tests for Dino pure logic (hitbox clearance + high-score board).
 * Not part of the firmware build — run with:
 *   gcc -O2 -Wall -Wextra -o /tmp/dino_host_test main/apps/dino/dino_host_test.c && /tmp/dino_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define FIELD_H         168
#define GROUND_Y        (FIELD_H - 18)
#define RUNNER_X        36
#define HI_N            5
#define NIGHT_EVERY     700

/* Soarer bands (mirrors dino_app.c spawn_obstacle) */
#define SOAR_HIGH_Y     (GROUND_Y - 30)
#define SOAR_MID_Y      (GROUND_Y - 20)
#define SOAR_LOW_Y      (GROUND_Y - 14)
#define SOAR_H          12
#define SOAR_W          26

static int fails;

static void expect(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    } else {
        printf("ok  %s\n", msg);
    }
}

static bool rects_overlap(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/* Mirrors dino_app.c duck / stand hitbox + fairness shrink. */
static void runner_hitbox(bool duck, int feet_y, int *x, int *y, int *w, int *h)
{
    *x = RUNNER_X;
    if (duck) {
        *y = feet_y - 12;
        *w = 26;
        *h = 12;
        *x += 2;
        *y += 3;
        *w -= 4;
        *h -= 4;
    } else {
        *y = feet_y - 26;
        *w = 22;
        *h = 26;
        *x += 2;
        *y += 2;
        *w -= 4;
        *h -= 3;
    }
}

static void hi_sort_desc(int32_t *hi)
{
    for (int i = 0; i < HI_N - 1; i++) {
        for (int j = i + 1; j < HI_N; j++) {
            if (hi[j] > hi[i]) {
                int32_t t = hi[i];
                hi[i] = hi[j];
                hi[j] = t;
            }
        }
    }
}

static int hi_insert(int32_t *hi, int32_t score)
{
    if (score <= 0) {
        return 0;
    }
    if (score <= hi[HI_N - 1] && hi[HI_N - 1] > 0) {
        return 0;
    }
    hi[HI_N - 1] = score;
    hi_sort_desc(hi);
    for (int i = 0; i < HI_N; i++) {
        if (hi[i] == score) {
            return i + 1;
        }
    }
    return 0;
}

static bool night_at(int score)
{
    return ((score / NIGHT_EVERY) % 2) == 1;
}

int main(void)
{
    int dx, dy, dw, dh;
    int sx, sy, sw, sh;

    runner_hitbox(true, GROUND_Y, &dx, &dy, &dw, &dh);
    runner_hitbox(false, GROUND_Y, &sx, &sy, &sw, &sh);

    expect(dh >= 4 && sh >= 4, "hitboxes have positive height after shrink");
    expect(dy > sy, "duck AABB top is lower than standing (tighter crouch)");
    expect(dh < sh, "duck height shorter than standing");

    /* High soarer: stand hits, duck clears */
    expect(rects_overlap(sx, sy, sw, sh, RUNNER_X, SOAR_HIGH_Y, SOAR_W, SOAR_H),
           "standing hits high soarer");
    expect(!rects_overlap(dx, dy, dw, dh, RUNNER_X, SOAR_HIGH_Y, SOAR_W, SOAR_H),
           "duck clears high soarer");

    /* Mid / low: both postures collide → must jump */
    expect(rects_overlap(sx, sy, sw, sh, RUNNER_X, SOAR_MID_Y, SOAR_W, SOAR_H),
           "standing hits mid soarer");
    expect(rects_overlap(dx, dy, dw, dh, RUNNER_X, SOAR_MID_Y, SOAR_W, SOAR_H),
           "duck still hits mid soarer (jump)");
    expect(rects_overlap(dx, dy, dw, dh, RUNNER_X, SOAR_LOW_Y, SOAR_W, SOAR_H),
           "duck hits low soarer (jump)");

    /* High-score board */
    int32_t hi[HI_N] = {0};
    expect(hi_insert(hi, 100) == 1, "first score is rank 1");
    expect(hi_insert(hi, 50) == 2, "second lower is rank 2");
    expect(hi_insert(hi, 200) == 1, "new high becomes rank 1");
    expect(hi[0] == 200 && hi[1] == 100 && hi[2] == 50, "board sorted desc");
    hi_insert(hi, 80);
    hi_insert(hi, 60);
    expect(hi_insert(hi, 10) == 0, "below #5 rejected when board full");
    expect(hi_insert(hi, 70) == 4, "70 inserts into board");

    /* Night mode cadence */
    expect(!night_at(0) && !night_at(699), "day before 700");
    expect(night_at(700) && night_at(1399), "night 700–1399");
    expect(!night_at(1400), "day again at 1400");

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
