#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../firmware/main/pixel_avatar.h"

int main(void) {
    uint16_t dst[192 * 192];
    uint16_t src[64 * 64];
    uint8_t mask[512];
    memset(src, 0, sizeof(src)); memset(mask, 0, sizeof(mask));
    src[0] = 0xf800; mask[0] = 0x80;
    kim_pixel_compose(dst, src, mask, 0x1234);
    assert(dst[0] == 0xf800 && dst[1] == 0xf800 && dst[192] == 0xf800);
    assert(dst[3] == 0x1234 && dst[192 * 3] == 0x1234);
    assert(kim_pixel_frame(KIM_ANIM_SPEAKING, 0, 0) == 32);
    assert(kim_pixel_frame(KIM_ANIM_SPEAKING, 0, 1) == 33);
    assert(kim_pixel_frame(KIM_ANIM_SPEAKING, 0, 2) == 34);
    assert(kim_pixel_frame(KIM_ANIM_SPEAKING, 75, 2) == 35);
    assert(kim_pixel_frame(KIM_ANIM_LISTENING, 240, 2) == 18);
    assert(kim_pixel_frame(KIM_ANIM_OFFLINE, 100000, 2) == 45);
    assert(kim_pixel_bank(KIM_LISTENING, 1, 0) == KIM_ANIM_LISTENING);
    assert(kim_pixel_bank(KIM_THINKING, 1, 0) == KIM_ANIM_THINKING);
    assert(kim_pixel_bank(KIM_SLEEP, 1, 0) == KIM_ANIM_SLEEP);
    assert(kim_pixel_bank(KIM_OFFLINE, 1, 0) == KIM_ANIM_OFFLINE);
    assert(kim_pixel_bank(KIM_SPEAKING, 1, 0) == KIM_ANIM_SPEAKING);
    assert(kim_pixel_bank(KIM_IDLE, 1, 0) == KIM_ANIM_HAPPY);
    assert(kim_pixel_bank(KIM_IDLE, 0, 0) == KIM_ANIM_IDLE);
    assert(kim_pixel_bank(KIM_IDLE, 0, 1) == KIM_ANIM_BLINK);
    return 0;
}
