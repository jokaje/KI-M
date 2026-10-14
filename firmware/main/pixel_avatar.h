#pragma once
#include <stdint.h>
#include "generated/sprites.h"

/* Supplied straight-alpha 64x64 sprites, 3x nearest-neighbor to 192x192.
 * Masks are one bit per pixel, MSB first; caller owns a 192x192 RGB565 buffer. */
void kim_pixel_compose(uint16_t *out, const uint16_t *rgb, const uint8_t *mask, uint16_t background);
unsigned kim_pixel_frame(kim_anim_bank_t bank, uint32_t elapsed_ms, unsigned mouth);
kim_anim_bank_t kim_pixel_bank(int state, int emotion, int blink);
