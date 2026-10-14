#include "kim_trial.h"
#include <string.h>
void kim_trial_start(kim_trial_t *t, bool pending, uint32_t now) {
    atomic_store(&t->pending, pending); atomic_store(&t->captured, false);
    atomic_store(&t->replied, false); atomic_store(&t->menu_opened, false);
    atomic_store(&t->menu_cycle, false); t->started_ms = now;
}
void kim_trial_capture(kim_trial_t *t, bool nonzero) {
    if (t->pending) t->captured = nonzero;
}
void kim_trial_reply(kim_trial_t *t, bool nonzero_pcm, bool complete) {
    if (t->pending && t->captured && nonzero_pcm && complete) t->replied = true;
}
void kim_trial_menu(kim_trial_t *t, bool open) {
    if (!t->pending) return;
    if (open) t->menu_opened = true;
    else if (t->menu_opened) t->menu_cycle = true;
}
bool kim_trial_ready(const kim_trial_t *t, bool display, bool audio, bool wifi, bool backend) {
    return t->pending && t->captured && t->replied && t->menu_cycle &&
           display && audio && wifi && backend;
}
bool kim_trial_expired(const kim_trial_t *t, uint32_t now) {
    return t->pending && (uint32_t)(now - t->started_ms) >= 180000U;
}
