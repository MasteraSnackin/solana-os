// Solana OS / BP26 platform layer for doomgeneric (CMAP256 → RGB565).

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"

#include "apps/emu_pad.h"
#include "doom_host.h"

#define KEYQ 32

typedef struct {
    uint8_t pressed;
    uint8_t key;
} key_ev_t;

static key_ev_t s_q[KEYQ];
static int s_q_r, s_q_w;
static uint32_t s_prev_joy;
static uint16_t *s_rgb;                 // 320x200 RGB565 for the host
static volatile int s_frame_ready;

static void q_push(int pressed, unsigned char key)
{
    int n = (s_q_w + 1) % KEYQ;
    if (n == s_q_r) {
        return;
    }
    s_q[s_q_w].pressed = (uint8_t)pressed;
    s_q[s_q_w].key = key;
    s_q_w = n;
}

static void joy_edge(uint32_t now, uint32_t bit, unsigned char key)
{
    bool on = (now & bit) != 0;
    bool was = (s_prev_joy & bit) != 0;
    if (on == was) {
        return;
    }
    q_push(on ? 1 : 0, key);
}

void doom_solana_set_fb(uint16_t *rgb)
{
    s_rgb = rgb;
}

void doom_solana_feed_joy(uint32_t joy)
{
    // Doom defaults: arrows move, Ctrl fire, space use, Enter select, Esc menu.
    joy_edge(joy, EMU_PAD_UP, KEY_UPARROW);
    joy_edge(joy, EMU_PAD_DOWN, KEY_DOWNARROW);
    joy_edge(joy, EMU_PAD_LEFT, KEY_LEFTARROW);
    joy_edge(joy, EMU_PAD_RIGHT, KEY_RIGHTARROW);
    joy_edge(joy, EMU_PAD_A, KEY_FIRE);       // badge SELECT
    joy_edge(joy, EMU_PAD_B, KEY_USE);        // badge CANCEL
    joy_edge(joy, EMU_PAD_START, KEY_ENTER);  // UP+DOWN / SELECT+CANCEL
    joy_edge(joy, EMU_PAD_SELECT, KEY_ESCAPE);// LEFT+RIGHT
    s_prev_joy = joy;
}

int doom_solana_take_frame(void)
{
    int r = s_frame_ready;
    s_frame_ready = 0;
    return r;
}

void doom_solana_reset_input(void)
{
    s_q_r = s_q_w = 0;
    s_prev_joy = 0;
    s_frame_ready = 0;
}

void DG_Init(void)
{
    doom_solana_reset_input();
}

void DG_DrawFrame(void)
{
    if (!s_rgb || !DG_ScreenBuffer) {
        return;
    }
    const pixel_t *src = DG_ScreenBuffer;
    uint16_t *dst = s_rgb;
    for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) {
        struct color c = colors[src[i]];
        dst[i] = (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
    }
    s_frame_ready = 1;
}

void DG_SleepMs(uint32_t ms)
{
    if (ms == 0) {
        taskYIELD();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (s_q_r == s_q_w) {
        return 0;
    }
    *pressed = s_q[s_q_r].pressed;
    *key = s_q[s_q_r].key;
    s_q_r = (s_q_r + 1) % KEYQ;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}


/* ESP-DOOM CMAP256 hooks: palette is already applied via I_SetPalette → colors[]. */
void DG_PaletteLoad(const void *playpal)
{
    (void)playpal;
}

uint16_t DG_PaletteColor565(uint8_t index)
{
    struct color c = colors[index];
    return (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
}
