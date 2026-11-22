#pragma once

// Chorded pad for gbemu / gbaemu / snesemu on the six-key badge (no Start, no L/R).
// Opposite d-pad keys latch: UP+DOWN is START (not UP then DOWN), LEFT+RIGHT
// is SELECT. SELECT+CANCEL is a second START. LEFT+RIGHT+UP/DOWN are L/R.
// BOOT stays HOME.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "core/os.h"

#define EMU_PAD_A       (1u << 0)
#define EMU_PAD_B       (1u << 1)
#define EMU_PAD_SELECT  (1u << 2)
#define EMU_PAD_START   (1u << 3)
#define EMU_PAD_RIGHT   (1u << 4)
#define EMU_PAD_LEFT    (1u << 5)
#define EMU_PAD_UP      (1u << 6)
#define EMU_PAD_DOWN    (1u << 7)
#define EMU_PAD_R       (1u << 8)
#define EMU_PAD_L       (1u << 9)

#define EMU_PAD_GRACE_US  100000LL

typedef struct {
    bool up, down, left, right, a, b;
    bool dpad_start;
    bool dpad_select;
    bool face_start;
    bool exit_chord;
    bool save_chord;
    bool l, r;
    int64_t ud_since, lr_since;
    bool start_latched, select_latched;
    uint32_t last_logged;
} emu_pad_t;

static inline void emu_pad_reset(emu_pad_t *p)
{
    memset(p, 0, sizeof(*p));
}

static inline void emu_pad_sample(emu_pad_t *p)
{
    uint32_t b = bsp_buttons_get();
    int64_t now = esp_timer_get_time();
    p->up = b & os_key_button(OS_KEY_UP);
    p->down = b & os_key_button(OS_KEY_DOWN);
    p->left = b & os_key_button(OS_KEY_LEFT);
    p->right = b & os_key_button(OS_KEY_RIGHT);
    p->a = b & os_key_button(OS_KEY_SELECT);
    p->b = b & os_key_button(OS_KEY_BACK);

    if (p->up && p->down) {
        p->start_latched = true;
        p->ud_since = 0;
    } else if (!p->up && !p->down) {
        p->start_latched = false;
        p->ud_since = 0;
    } else if (!p->start_latched && !p->ud_since) {
        p->ud_since = now;
    }

    if (p->left && p->right) {
        p->select_latched = true;
        p->lr_since = 0;
    } else if (!p->left && !p->right) {
        p->select_latched = false;
        p->lr_since = 0;
    } else if (!p->select_latched && !p->lr_since) {
        p->lr_since = now;
    }

    p->dpad_start = p->start_latched;
    p->dpad_select = p->select_latched;
    p->face_start = p->a && p->b;
    p->exit_chord = p->a && p->dpad_start;
    p->save_chord = p->a && p->dpad_select;
    p->l = p->dpad_select && p->up && !p->down;
    p->r = p->dpad_select && p->down && !p->up;
}

static inline uint32_t emu_pad_joy(const emu_pad_t *p)
{
    uint32_t held = os_pad_injected();
    int64_t now = esp_timer_get_time();

    if (p->exit_chord || p->save_chord) {
        return held;
    }

    bool start = p->dpad_start || p->face_start;
    if (start) {
        held |= EMU_PAD_START;
    } else if (!(p->l || p->r)) {
        bool grace = (p->up || p->down) && p->ud_since &&
                     (now - p->ud_since < EMU_PAD_GRACE_US);
        if (!grace) {
            held |= (p->up ? EMU_PAD_UP : 0) | (p->down ? EMU_PAD_DOWN : 0);
        }
    }

    if (p->l) {
        held |= EMU_PAD_L;
    } else if (p->r) {
        held |= EMU_PAD_R;
    } else if (p->dpad_select) {
        held |= EMU_PAD_SELECT;
    } else {
        bool grace = (p->left || p->right) && p->lr_since &&
                     (now - p->lr_since < EMU_PAD_GRACE_US);
        if (!grace) {
            held |= (p->left ? EMU_PAD_LEFT : 0) | (p->right ? EMU_PAD_RIGHT : 0);
        }
    }

    if (p->a && !p->face_start) {
        held |= EMU_PAD_A;
    }
    if (p->b && !p->face_start) {
        held |= EMU_PAD_B;
    }
    return held;
}

static inline void emu_pad_log(emu_pad_t *p, uint32_t held, const char *tag)
{
    uint32_t edge = EMU_PAD_START | EMU_PAD_L | EMU_PAD_R;
    if ((held & edge) == (p->last_logged & edge)) {
        return;
    }
    ESP_LOGI(tag, "pad %03x%s%s%s", (unsigned)held,
             (held & EMU_PAD_START) ? " START" : "",
             (held & EMU_PAD_L) ? " L" : "",
             (held & EMU_PAD_R) ? " R" : "");
    p->last_logged = held;
}
