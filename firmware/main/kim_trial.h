#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
/* A boot-local OTA trial; factory images are never enrolled. */
typedef struct {
    atomic_bool pending, captured, replied, menu_opened, menu_cycle;
    uint32_t started_ms;
} kim_trial_t;
void kim_trial_start(kim_trial_t *trial, bool pending, uint32_t now);
void kim_trial_capture(kim_trial_t *trial, bool nonzero);
void kim_trial_reply(kim_trial_t *trial, bool nonzero_pcm, bool complete);
void kim_trial_menu(kim_trial_t *trial, bool open);
bool kim_trial_ready(const kim_trial_t *trial, bool display, bool audio, bool wifi, bool backend);
bool kim_trial_expired(const kim_trial_t *trial, uint32_t now);
