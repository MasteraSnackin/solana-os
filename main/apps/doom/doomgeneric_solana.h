#pragma once

#include <stdint.h>

void doom_solana_set_fb(uint16_t *rgb);
void doom_solana_feed_joy(uint32_t joy);
int doom_solana_take_frame(void);
void doom_solana_reset_input(void);
