#include "kim_character_state.h"
#include <string.h>

/* Deliberately restricted JSON grammar. This metadata has exactly two scalar
 * fields; declining extensions is safer than accidentally accepting thoughts. */
typedef struct { const char *p, *end; } cursor_t;
static void space(cursor_t *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\n' || *c->p == '\r' || *c->p == '\t')) ++c->p;
}
static bool take(cursor_t *c, char ch) { space(c); if (c->p == c->end || *c->p != ch) return false; ++c->p; return true; }
static bool literal(cursor_t *c, const char *s) {
    space(c);
    size_t n = strlen(s);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, s, n)) return false;
    c->p += n;
    return true;
}
bool kim_character_parse(const char *json, size_t length, kim_character_state_t *out) {
    if (!json || !out || !length || length > 192) return false;
    cursor_t c = {json, json + length};
    unsigned seen = 0;
    kim_character_state_t state = KIM_CHARACTER_IDLE;
    if (!take(&c, '{')) return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (i && !take(&c, ',')) return false;
        if (literal(&c, "\"version\"")) {
            if (seen & 1 || !take(&c, ':') || !literal(&c, "1")) return false;
            seen |= 1;
        } else if (literal(&c, "\"recommended_state\"")) {
            if (seen & 2 || !take(&c, ':')) return false;
            if (literal(&c, "\"idle\"")) state = KIM_CHARACTER_IDLE;
            else if (literal(&c, "\"thinking\"")) state = KIM_CHARACTER_THINKING;
            else if (literal(&c, "\"happy\"")) state = KIM_CHARACTER_HAPPY;
            else if (literal(&c, "\"sleep\"")) state = KIM_CHARACTER_SLEEP;
            else return false;
            seen |= 2;
        } else return false;
    }
    if (seen != 3 || !take(&c, '}')) return false;
    space(&c);
    if (c.p != c.end) return false;
    *out = state;
    return true;
}
bool kim_character_eligible(const kim_character_gate_t *g) {
    return g && g->online && g->healthy && g->idle && g->released && g->menu_allowed && g->unmuted;
}
bool kim_character_due(const kim_character_poll_t *p, uint32_t now, const kim_character_gate_t *g) {
    return kim_character_eligible(g) && (!atomic_load(&p->polled) ||
        (uint32_t)(now - atomic_load(&p->last_poll_ms)) >= 10000U);
}
void kim_character_poll_started(kim_character_poll_t *p, uint32_t now) {
    atomic_store(&p->last_poll_ms, now);
    atomic_store(&p->polled, true);
}
void kim_character_interact(kim_character_poll_t *p) {
    atomic_fetch_add(&p->generation, 1);
    atomic_store(&p->pending, 0);
    atomic_store(&p->suggestion, KIM_CHARACTER_IDLE);
}
void kim_character_offer(kim_character_poll_t *p, unsigned generation, kim_character_state_t state) {
    if (state < KIM_CHARACTER_IDLE || state > KIM_CHARACTER_SLEEP ||
        generation != atomic_load(&p->generation)) return;
    atomic_store_explicit(&p->pending, ((uint64_t)generation << 3) | (unsigned)(state + 1),
                          memory_order_release);
}
bool kim_character_consume(kim_character_poll_t *p, uint32_t now, const kim_character_gate_t *gate) {
    /* Called only by the LVGL/input writer; no network thread writes visuals. */
    uint64_t value = atomic_exchange_explicit(&p->pending, 0, memory_order_acquire);
    if (!value || (value & 7) > 4) return false;
    return kim_character_publish(p, (unsigned)(value >> 3),
                                 (kim_character_state_t)((value & 7) - 1), now, gate);
}
bool kim_character_publish(kim_character_poll_t *p, unsigned generation, kim_character_state_t state,
                           uint32_t now, const kim_character_gate_t *g) {
    if (!kim_character_eligible(g) || generation != atomic_load(&p->generation) ||
        state < KIM_CHARACTER_IDLE || state > KIM_CHARACTER_SLEEP) return false;
    if (state == KIM_CHARACTER_THINKING &&
        ((kim_character_state_t)atomic_load(&p->suggestion) != KIM_CHARACTER_THINKING ||
         atomic_load(&p->suggestion_generation) != generation))
        atomic_store(&p->thinking_since_ms, now);
    atomic_store(&p->suggestion, state);
    atomic_store(&p->received_ms, now);
    atomic_store(&p->suggestion_generation, generation);
    return true;
}
kim_character_state_t kim_character_visible(const kim_character_poll_t *p, uint32_t now,
                                             const kim_character_gate_t *g) {
    if (!kim_character_eligible(g) || atomic_load(&p->suggestion_generation) != atomic_load(&p->generation) ||
        (uint32_t)(now - atomic_load(&p->received_ms)) > 30000U) return KIM_CHARACTER_IDLE;
    kim_character_state_t state = (kim_character_state_t)atomic_load(&p->suggestion);
    if (state == KIM_CHARACTER_THINKING &&
        (uint32_t)(now - atomic_load(&p->thinking_since_ms)) > 20000U) return KIM_CHARACTER_IDLE;
    return state;
}
