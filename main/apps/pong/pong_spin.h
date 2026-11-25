// Pure paddle-edge spin math for Pong (host-testable, no ESP deps).
// Hit near the paddle center → small |vy|; near either edge → stronger spin.
#pragma once

#ifndef PONG_SPIN_MAX
#define PONG_SPIN_MAX 8
#endif

/** Vertical velocity from where the ball strikes the paddle.
 *  @param ball_cy   ball centre Y (court coords)
 *  @param paddle_y  paddle top Y
 *  @param paddle_h  paddle height (pixels)
 *  @return vy in [-PONG_SPIN_MAX, PONG_SPIN_MAX]; 0 only for a dead-centre hit
 */
static inline int pong_edge_spin(int ball_cy, int paddle_y, int paddle_h)
{
    if (paddle_h <= 0) {
        return 0;
    }
    int half = paddle_h / 2;
    if (half <= 0) {
        return 0;
    }
    int off = ball_cy - (paddle_y + half); /* negative = upper edge */
    if (off > half) {
        off = half;
    } else if (off < -half) {
        off = -half;
    }

    /* Linear spin scaled to paddle half-height. */
    int vy = (off * PONG_SPIN_MAX) / half;

    /* Outer-third edge boost — classic arcade “graze” spin. */
    int edge = (half * 2) / 3;
    if (off > edge) {
        vy += 2;
    } else if (off < -edge) {
        vy -= 2;
    }

    if (vy > PONG_SPIN_MAX) {
        vy = PONG_SPIN_MAX;
    } else if (vy < -PONG_SPIN_MAX) {
        vy = -PONG_SPIN_MAX;
    }
    return vy;
}
