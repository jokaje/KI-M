#include "barge_gate.h"
void kim_barge_gate_reset(kim_barge_gate_t *g) { *g = (kim_barge_gate_t){0}; }
bool kim_barge_gate_feed(kim_barge_gate_t *g, bool speech, unsigned ms,
                         bool muted, bool playing) {
    if (muted || !playing || ms == 0 || ms > 40) {
        kim_barge_gate_reset(g);
        return false;
    }
    if (speech) {
        g->silence_ms = 0;
        g->speech_ms = g->speech_ms + ms > 1000 ? 1000 : g->speech_ms + ms;
        if (g->speech_ms >= 180) g->triggered = true;
    } else {
        g->silence_ms = g->silence_ms + ms > 1000 ? 1000 : g->silence_ms + ms;
        if (g->silence_ms >= 80) g->speech_ms = 0;
    }
    return g->triggered;
}
