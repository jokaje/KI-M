#include "handsfree.h"

#include <string.h>

/* The endpointer is byte-order agnostic: it samples absolute amplitudes
 * through kim_vad_feed, so it accepts the same LE byte stream the codec
 * delivers and never rewrites audio contents. */

static void preroll_store(kim_handsfree_t *h, const uint8_t *frame) {
    if (h->preroll_valid < KIM_HF_PREROLL_FRAMES) ++h->preroll_valid;
    memcpy(h->preroll + h->preroll_next * KIM_HF_FRAME_BYTES, frame, KIM_HF_FRAME_BYTES);
    h->preroll_next = (h->preroll_next + 1) % KIM_HF_PREROLL_FRAMES;
}

static void preroll_flush(kim_handsfree_t *h) {
    /* Copy ring oldest-first into out; the transitioning frame is already
     * in the ring, so it is delivered exactly once. */
    size_t base = 0;
    unsigned start = h->preroll_valid < KIM_HF_PREROLL_FRAMES
        ? 0 : h->preroll_next;
    for (unsigned i = 0; i < h->preroll_valid; ++i) {
        unsigned idx = (start + i) % KIM_HF_PREROLL_FRAMES;
        memcpy(h->out + base, h->preroll + idx * KIM_HF_FRAME_BYTES, KIM_HF_FRAME_BYTES);
        base += KIM_HF_FRAME_BYTES;
    }
    h->out_len = base;
    h->preroll_valid = 0;
    h->preroll_next = 0;
}

static bool append_pcm(kim_handsfree_t *h, const uint8_t *frame) {
    if (h->out_len + KIM_HF_FRAME_BYTES > h->out_cap) return false;
    memcpy(h->out + h->out_len, frame, KIM_HF_FRAME_BYTES);
    h->out_len += KIM_HF_FRAME_BYTES;
    return true;
}

static void drop_turn(kim_handsfree_t *h) {
    /* Noise or an unusable buffer: register nothing, restart cleanly. */
    memset(h->out, 0, h->out_len); /* raw audio never persists */
    h->out_len = 0;
    h->voiced = 0;
    h->speech_ms = 0;
    h->end_silence = 0;
    h->state = KIM_HF_IDLE; /* pre-roll ring re-arms from silence */
}

void kim_handsfree_init(kim_handsfree_t *h, uint8_t *out, size_t out_cap) {
    memset(h, 0, sizeof(*h));
    kim_vad_reset(&h->vad);
    h->out = out;
    h->out_cap = out_cap - (out_cap % KIM_HF_FRAME_BYTES);
}

static bool endpoint_ready(kim_handsfree_t *h) {
    return h->voiced >= KIM_HF_MIN_VOICED && h->end_silence >= KIM_HF_END_SILENCE;
}

bool kim_handsfree_frame(kim_handsfree_t *h, const uint8_t *frame320) {
    if (!h || !h->out || h->state == KIM_HF_COMPLETE) return false;
    int16_t samples[KIM_HF_FRAME_SAMPLES];
    for (int i = 0; i < KIM_HF_FRAME_SAMPLES; ++i)
        samples[i] = (int16_t)(frame320[2 * i] | ((uint16_t)frame320[2 * i + 1] << 8));
    bool voiced = kim_vad_feed(&h->vad, samples, KIM_HF_FRAME_SAMPLES);
    /* Per-frame genuine loudness: the VAD keeps speech=true through its
     * 450-ms trailing decay, so it cannot drive the frame counter. A quiet
     * frame resets active_frames to 0 in the VAD; a loud one raises it. */
    bool loud_now = h->vad.active_frames > 0;

    switch (h->state) {
    case KIM_HF_IDLE:
        preroll_store(h, frame320); /* transition frame rides in the ring */
        if (voiced) {
            h->state = KIM_HF_SPEECH;
            h->voiced = 1;
            h->speech_ms = 10;
            h->end_silence = 0;
            preroll_flush(h); /* onset audio is preserved, not lost */
        }
        break;
    case KIM_HF_SPEECH:
        /* Count GENUINELY voiced frames only: the raw VAD stays true for
         * its 450-ms decay trailing silence; counting those frames as
         * voiced would inflate the speech gate forever. Each silence
         * frame after speech start is still appended (context), but does
         * not add to `voiced`. */
        if (loud_now) {
            h->end_silence = 0;
            ++h->voiced;
            if (h->speech_ms < KIM_HF_MAX_SPEECH_MS) h->speech_ms += 10;
        } else if (h->end_silence < KIM_HF_END_SILENCE) {
            ++h->end_silence;
        }
        if (!append_pcm(h, frame320)) {
            h->state = KIM_HF_COMPLETE; /* close out at buffer cap, no overrun */
            break;
        }
        if (h->speech_ms >= KIM_HF_MAX_SPEECH_MS || endpoint_ready(h)) {
            h->state = KIM_HF_COMPLETE;
        } else if (h->end_silence >= KIM_HF_END_SILENCE && h->voiced < KIM_HF_MIN_VOICED) {
            drop_turn(h); /* short noise burst: no send, back to listening */
        }
        break;
    default:
        break;
    }
    if (h->state != KIM_HF_COMPLETE && h->listen_ms < KIM_HF_MAX_LISTEN_MS)
        h->listen_ms += 10;
    return h->state == KIM_HF_COMPLETE && h->voiced >= KIM_HF_MIN_VOICED &&
           h->out_len >= KIM_HF_FRAME_BYTES;
}

kim_handsfree_state_t kim_handsfree_state(const kim_handsfree_t *h) { return h->state; }
size_t kim_handsfree_length(const kim_handsfree_t *h) { return h->out_len; }
unsigned kim_handsfree_voiced(const kim_handsfree_t *h) { return h->voiced; }
