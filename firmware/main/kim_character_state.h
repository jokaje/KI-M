#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum { KIM_CHARACTER_IDLE, KIM_CHARACTER_THINKING, KIM_CHARACTER_HAPPY, KIM_CHARACTER_SLEEP } kim_character_state_t;
typedef struct {
    bool online, healthy, idle, released, menu_allowed, unmuted;
} kim_character_gate_t;
typedef struct {
    atomic_uint generation;
    atomic_uint suggestion_generation;
    atomic_int suggestion;
    atomic_uint received_ms;
    atomic_uint thinking_since_ms;
    atomic_uint last_poll_ms;
    atomic_bool polled;
    /* (generation << 3) | (state + 1); zero means no pending result. */
    atomic_uint_fast64_t pending;
} kim_character_poll_t;
/* Exact allowlist: no private text, unknown fields, duplicate keys, or trailing bytes. */
bool kim_character_parse(const char *json, size_t length, kim_character_state_t *out);
bool kim_character_eligible(const kim_character_gate_t *gate);
bool kim_character_due(const kim_character_poll_t *p, uint32_t now, const kim_character_gate_t *gate);
void kim_character_poll_started(kim_character_poll_t *p, uint32_t now);
void kim_character_interact(kim_character_poll_t *p);
void kim_character_offer(kim_character_poll_t *p, unsigned generation, kim_character_state_t state);
bool kim_character_consume(kim_character_poll_t *p, uint32_t now, const kim_character_gate_t *gate);
bool kim_character_publish(kim_character_poll_t *p, unsigned generation, kim_character_state_t state,
                           uint32_t now, const kim_character_gate_t *gate);
kim_character_state_t kim_character_visible(const kim_character_poll_t *p, uint32_t now,
                                             const kim_character_gate_t *gate);
