#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "generated/avatar.h"

typedef struct {
    atomic_uint desired, desired_epoch, ready, ready_epoch, phase;
    uint16_t *front, *back;

    atomic_uint shown, shown_epoch;
    atomic_bool shown_valid;
    atomic_uint decoded, failed, presented, discarded; /* sanitized bounded diagnostics */
    atomic_uint decode_us_max, decode_us_total, decode_samples;
} kim_avatar_player_t;

/* Called by the LVGL writer only. Worker writes only back, and waits for
 * phase=0 before another decode. phase: 0 free, 1 decoding, 2 ready. */
void kim_avatar_request(kim_avatar_player_t *p, unsigned clip, uint32_t elapsed_ms, uint32_t epoch);
bool kim_avatar_present(kim_avatar_player_t *p);

void kim_avatar_release(kim_avatar_player_t *p);
const uint16_t *kim_avatar_display_data(const kim_avatar_player_t *p, const uint16_t *fallback);
bool kim_avatar_begin_decode(kim_avatar_player_t *p, uint32_t pack_size, unsigned *index, unsigned *epoch);
void kim_avatar_finish_decode(kim_avatar_player_t *p, unsigned index, unsigned epoch, bool success);
bool kim_avatar_copy_rgb_rect(uint16_t *pixels, const uint8_t *rgb,
                              unsigned left, unsigned top, unsigned right, unsigned bottom);
void kim_avatar_worker(void *arg);
bool kim_avatar_frame_valid(unsigned index, uint32_t pack_size);
unsigned kim_avatar_select_clip(int state, int emotion, int sleep_phase);
uint32_t kim_avatar_elapsed(unsigned clip, int sleep_phase, uint32_t sleep_elapsed,
                            uint32_t now_ms, uint32_t clip_started);
