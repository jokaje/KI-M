#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Epoch is monotonic within one boot. Receiver must reset its epoch baseline
 * on a new boot/session or after lease expiry; it must not treat it as a
 * persistent cross-reboot counter. No audio or private thought is serialized. */
#define KIM_DEVICE_STATE_JSON_CAP 96
#define KIM_DEVICE_STATE_PERIOD_MS 5000U
typedef struct {
    uint32_t epoch, last_sent_ms, retry_after_ms;
    bool sampled, acknowledged, muted, menu_open;
} kim_device_state_t;
bool kim_device_state_sample(kim_device_state_t *s, uint32_t now, bool muted, bool menu_open);
bool kim_device_state_due(const kim_device_state_t *s, uint32_t now);
uint32_t kim_device_state_epoch(const kim_device_state_t *s);
bool kim_device_state_format(const kim_device_state_t *s, uint32_t epoch, char *out, size_t cap);
void kim_device_state_sent(kim_device_state_t *s, uint32_t now, uint32_t epoch);
void kim_device_state_failed(kim_device_state_t *s, uint32_t now);
