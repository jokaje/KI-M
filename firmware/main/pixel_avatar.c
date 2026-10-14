#include "pixel_avatar.h"
#include <stddef.h>

void kim_pixel_compose(uint16_t *out, const uint16_t *rgb, const uint8_t *mask, uint16_t background) {
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            size_t i = (size_t)y * 64 + x;
            uint16_t color = (mask[i >> 3] & (0x80u >> (i & 7))) ? rgb[i] : background;
            size_t base = (size_t)y * 3 * 192 + x * 3;
            for (unsigned row = 0; row < 3; ++row) {
                uint16_t *dst = out + base + row * 192;
                dst[0] = dst[1] = dst[2] = color;
            }
        }
    }
}

unsigned kim_pixel_frame(kim_anim_bank_t bank, uint32_t elapsed_ms, unsigned mouth) {
    if ((unsigned)bank >= KIM_ANIM_COUNT) bank = KIM_ANIM_OFFLINE;
    const kim_anim_meta_t *meta = &kim_anim_table[bank];
    unsigned local;
    if (bank == KIM_ANIM_SPEAKING)
        local = mouth == 0 ? 0 : mouth == 1 ? 1 : 2 + (elapsed_ms / meta->default_period_ms) % 2;
    else {
        local = elapsed_ms / meta->default_period_ms;
        if (meta->loop) local %= meta->frame_count;
        else if (local >= meta->frame_count) local = meta->frame_count - 1;
    }
    return meta->frame_offset + local;
}

kim_anim_bank_t kim_pixel_bank(int state, int emotion, int blink) {
    if (state == KIM_OFFLINE) return KIM_ANIM_OFFLINE;
    if (state == KIM_SLEEP) return KIM_ANIM_SLEEP;
    if (state == KIM_LISTENING) return KIM_ANIM_LISTENING;
    if (state == KIM_THINKING) return KIM_ANIM_THINKING;
    if (state == KIM_SPEAKING) return KIM_ANIM_SPEAKING;
    if (state == KIM_IDLE && emotion == 1) return KIM_ANIM_HAPPY;
    if (state == KIM_IDLE && emotion == 2) return KIM_ANIM_NEUTRAL; /* no authored crying pixel bank */
    if (state == KIM_IDLE && blink) return KIM_ANIM_BLINK;
    return KIM_ANIM_IDLE;
}
