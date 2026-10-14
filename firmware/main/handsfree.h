#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "voice_activity.h"

/* Realtime hands-free endpointer (pure state machine, no codec/HTTP here).
 * Half-duplex by design: the caller opens the mic, feeds 10 ms frames and
 * closes the mic. No AEC, no barge-in. Raw audio lives only in the
 * caller-provided buffer and is never persisted. */

#define KIM_HF_FRAME_SAMPLES   160   /* 10 ms at 16 kHz mono */
#define KIM_HF_FRAME_BYTES     320   /* s16le frame size */
#define KIM_HF_PREROLL_FRAMES  30    /* 300 ms kept before first voiced frame */
#define KIM_HF_MIN_VOICED      6     /* ~60 ms speech to validate a turn */
#define KIM_HF_END_SILENCE     45    /* ~450 ms trailing silence ends a turn */
#define KIM_HF_MAX_SPEECH_MS   8000  /* cap measured from SPEECH START */
#define KIM_HF_MAX_LISTEN_MS   15000 /* overall listen window then re-arm */

typedef enum {
    KIM_HF_IDLE = 0,     /* pre-roll ring active, waiting for voice */
    KIM_HF_SPEECH,       /* capturing; utterance growing from speech start */
    KIM_HF_COMPLETE      /* endpoint reached; buffer valid until next init */
} kim_handsfree_state_t;

typedef struct {
    kim_handsfree_state_t state;
    kim_vad_t vad;
    uint8_t preroll[KIM_HF_PREROLL_FRAMES * KIM_HF_FRAME_BYTES]; /* 9.6 KB */
    unsigned preroll_valid;   /* frames actually buffered (<= PREROLL) */
    unsigned preroll_next;    /* ring write cursor in frames */
    uint8_t *out;             /* caller capture buffer (PSRAM), written only by frame() */
    size_t out_cap, out_len;
    unsigned voiced, speech_ms, listen_ms, end_silence;
} kim_handsfree_t;

/* Bind the capture buffer. out may live in PSRAM; it is written by frame()
 * and cleared by the caller after sending. Re-init re-arms the endpointer
 * and discards any previous COMPLETE content ownership. */
void kim_handsfree_init(kim_handsfree_t *h, uint8_t *out, size_t out_cap);

/* Feed one 320-byte s16le frame (LE byte order as read from the codec).
 * Returns true exactly once per utterance when the endpoint fired; then
 * out_len holds the whole utterance INCLUDING pre-roll. Frames after true
 * are ignored until the next init. Never returns true before the caller's
 * min-speech gate; the first voiced audio is preserved via pre-roll. */
bool kim_handsfree_frame(kim_handsfree_t *h, const uint8_t *frame320);

kim_handsfree_state_t kim_handsfree_state(const kim_handsfree_t *h);
size_t kim_handsfree_length(const kim_handsfree_t *h);
unsigned kim_handsfree_voiced(const kim_handsfree_t *h);
