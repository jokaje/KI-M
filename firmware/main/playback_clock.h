#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* 16 kHz, mono s16le. BSP I2S: six 240-frame DMA descriptors (90 ms).
 * A successful codec write queues data; it does not await the DAC. */
#define KIM_PLAYBACK_RATE_BYTES 32000
#define KIM_PLAYBACK_DMA_US 90000

typedef struct {
    atomic_int_fast64_t until_us;
    atomic_uint level;
} kim_playback_clock_t;

void kim_playback_reset(kim_playback_clock_t *clock);
void kim_playback_written(kim_playback_clock_t *clock, int64_t now_us, unsigned bytes, unsigned level);
bool kim_playback_audible(const kim_playback_clock_t *clock, int64_t now_us);
unsigned kim_playback_level(const kim_playback_clock_t *clock, int64_t now_us);
int64_t kim_playback_remaining(const kim_playback_clock_t *clock, int64_t now_us);
/* Integer RMS bins for signed little-endian 16-bit PCM after a successful write. */
unsigned kim_playback_rms_level(const uint8_t *pcm, unsigned bytes);
