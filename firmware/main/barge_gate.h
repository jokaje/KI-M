#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Conservative speech gate for a future full-duplex AFE pipeline.
 * Input MUST be AEC-processed microphone PCM, not raw microphone PCM. */
typedef struct {
    unsigned speech_ms;
    unsigned silence_ms;
    bool triggered;
} kim_barge_gate_t;
void kim_barge_gate_reset(kim_barge_gate_t *gate);
bool kim_barge_gate_feed(kim_barge_gate_t *gate, bool afe_speech, unsigned frame_ms,
                         bool hardware_muted, bool playback_active);
