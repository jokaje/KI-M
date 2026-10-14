#include "playback_clock.h"

unsigned kim_playback_rms_level(const uint8_t *pcm, unsigned bytes) {
    if (!pcm || !bytes || (bytes & 1)) return 0;
    uint64_t square_sum = 0;
    for (unsigned i = 0; i < bytes; i += 2) {
        int32_t sample = (int16_t)((unsigned)pcm[i] | ((unsigned)pcm[i + 1] << 8));
        square_sum += (uint64_t)(sample * sample);
    }
    uint64_t mean_square = square_sum / (bytes / 2);
    return mean_square > 3500u * 3500u ? 2 : mean_square > 1000u * 1000u ? 1 : 0;
}

void kim_playback_reset(kim_playback_clock_t *clock) {
    atomic_store(&clock->level, 0);
    atomic_store(&clock->until_us, 0);
}

void kim_playback_written(kim_playback_clock_t *clock, int64_t now_us, unsigned bytes, unsigned level) {
    /* The BSP DMA has 6 * 240 frames = 90 ms maximum queued output.
     * The codec's write blocks when the DMA ring is full, so never add a
     * whole frame duration after a blocked write. */
    int64_t end = atomic_load(&clock->until_us);
    if (end < now_us) end = now_us;
    end += (int64_t)bytes * 1000000 / KIM_PLAYBACK_RATE_BYTES;
    if (end > now_us + KIM_PLAYBACK_DMA_US) end = now_us + KIM_PLAYBACK_DMA_US;
    atomic_store(&clock->level, level);
    atomic_store(&clock->until_us, end);
}

int64_t kim_playback_remaining(const kim_playback_clock_t *clock, int64_t now_us) {
    int64_t end = atomic_load(&clock->until_us);
    return end > now_us ? end - now_us : 0;
}

bool kim_playback_audible(const kim_playback_clock_t *clock, int64_t now_us) {
    return kim_playback_remaining(clock, now_us) > 0;
}

unsigned kim_playback_level(const kim_playback_clock_t *clock, int64_t now_us) {
    return kim_playback_audible(clock, now_us) ? atomic_load(&clock->level) : 0;
}
