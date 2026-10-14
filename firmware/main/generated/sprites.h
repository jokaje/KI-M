/**
 * firmware/main/generated/sprites.h
 * Automatisch generiert vom KI-M 2D Pixel-Art Studio (64x64 px, 46 Frames)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define KIM_SPRITE_GRID_W       64
#define KIM_SPRITE_GRID_H       64
#define KIM_SPRITE_FRAME_BYTES  (64 * 64 * 2)
#define KIM_SPRITE_TOTAL_FRAMES 46
#define KIM_DISPLAY_UPSCALE     3

typedef enum {
    KIM_ANIM_IDLE = 0,
    KIM_ANIM_HAPPY,
    KIM_ANIM_LISTENING,
    KIM_ANIM_THINKING,
    KIM_ANIM_SPEAKING,
    KIM_ANIM_BLINK,
    KIM_ANIM_SLEEP,
    KIM_ANIM_NEUTRAL,
    KIM_ANIM_OFFLINE,
    KIM_ANIM_COUNT
} kim_anim_bank_t;

typedef struct {
    const char *name;
    uint8_t frame_count;
    uint16_t frame_offset;
    uint16_t default_period_ms;
    uint8_t loop;
} kim_anim_meta_t;

static const kim_anim_meta_t kim_anim_table[KIM_ANIM_COUNT] = {
    [KIM_ANIM_IDLE]      = { "idle",      8,  0, 140, 1 },
    [KIM_ANIM_HAPPY]     = { "happy",     8,  8,  95, 1 },
    [KIM_ANIM_LISTENING] = { "listening", 8, 16, 120, 1 },
    [KIM_ANIM_THINKING]  = { "thinking",  8, 24, 110, 1 },
    [KIM_ANIM_SPEAKING]  = { "speaking",  4, 32,  75, 1 },
    [KIM_ANIM_BLINK]     = { "blink",     4, 36,  55, 0 },
    [KIM_ANIM_SLEEP]     = { "sleep",     4, 40, 260, 1 },
    [KIM_ANIM_NEUTRAL]   = { "neutral",   1, 44, 500, 1 },
    [KIM_ANIM_OFFLINE]   = { "offline",   1, 45, 500, 1 },
};

/* Existing voice state ABI, independent of pixel bank ordering. */
typedef enum { KIM_NEUTRAL, KIM_IDLE, KIM_BLINK, KIM_LISTENING, KIM_THINKING, KIM_SPEAKING, KIM_HAPPY, KIM_OFFLINE, KIM_SLEEP } kim_state_t;
#define KIM_SPRITE_COUNT KIM_SPRITE_TOTAL_FRAMES
