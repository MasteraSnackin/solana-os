/* Host unit tests for paddle-edge spin. No ESP-IDF / LVGL required.
 *
 *   cc -std=c11 -Wall -Wextra -o /tmp/test_pong_spin \
 *      main/apps/pong/test_pong_spin.c && /tmp/test_pong_spin
 */
#include <stdio.h>
#include <stdlib.h>

#include "apps/pong/pong_spin.h"

#define PADDLE_H 36

static int g_fail;

static void expect_eq(const char *name, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", name, got, want);
        g_fail++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void expect_abs_ge(const char *name, int got, int min_abs)
{
    int a = got < 0 ? -got : got;
    if (a < min_abs) {
        fprintf(stderr, "FAIL %s: |%d| < %d\n", name, got, min_abs);
        g_fail++;
    } else {
        printf("ok   %s (|%d| >= %d)\n", name, got, min_abs);
    }
}

static void expect_sign(const char *name, int got, int sign)
{
    int ok = (sign > 0) ? (got > 0) : (sign < 0) ? (got < 0) : (got == 0);
    if (!ok) {
        fprintf(stderr, "FAIL %s: got %d, expected sign %d\n", name, got, sign);
        g_fail++;
    } else {
        printf("ok   %s (vy=%d)\n", name, got);
    }
}

int main(void)
{
    const int py = 50; /* paddle top */
    const int mid = py + PADDLE_H / 2;

    /* Dead centre → no spin. */
    expect_eq("centre", pong_edge_spin(mid, py, PADDLE_H), 0);

    /* Upper half → negative vy (toward top). */
    expect_sign("upper-mid", pong_edge_spin(mid - 6, py, PADDLE_H), -1);

    /* Lower half → positive vy. */
    expect_sign("lower-mid", pong_edge_spin(mid + 6, py, PADDLE_H), +1);

    /* Top edge stronger than near-centre. */
    {
        int near = pong_edge_spin(mid - 4, py, PADDLE_H);
        int edge = pong_edge_spin(py + 1, py, PADDLE_H);
        int an = near < 0 ? -near : near;
        int ae = edge < 0 ? -edge : edge;
        if (ae <= an) {
            fprintf(stderr, "FAIL top-edge-stronger: edge |%d| <= near |%d|\n",
                    ae, an);
            g_fail++;
        } else {
            printf("ok   top-edge-stronger (|%d| > |%d|)\n", ae, an);
        }
        expect_abs_ge("top-edge-maxish", edge, PONG_SPIN_MAX - 2);
        expect_sign("top-edge-sign", edge, -1);
    }

    /* Bottom edge. */
    {
        int edge = pong_edge_spin(py + PADDLE_H - 1, py, PADDLE_H);
        expect_sign("bottom-edge-sign", edge, +1);
        expect_abs_ge("bottom-edge-maxish", edge, PONG_SPIN_MAX - 2);
    }

    /* Clamp: ball above paddle still within spin max. */
    {
        int vy = pong_edge_spin(py - 40, py, PADDLE_H);
        expect_eq("clamp-above", vy, -PONG_SPIN_MAX);
    }
    {
        int vy = pong_edge_spin(py + PADDLE_H + 40, py, PADDLE_H);
        expect_eq("clamp-below", vy, PONG_SPIN_MAX);
    }

    /* Degenerate paddle. */
    expect_eq("zero-height", pong_edge_spin(mid, py, 0), 0);

    if (g_fail) {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
