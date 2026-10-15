#include "kim_device_state.h"
#include <stdio.h>

bool kim_device_state_sample(kim_device_state_t *s, uint32_t now, bool muted, bool menu_open) {
    (void)now;
    if (!s) return false;
    if (s->sampled && s->muted == muted && s->menu_open == menu_open) return false;
    s->sampled = true;
    s->muted = muted;
    s->menu_open = menu_open;
    /* Never wrap the epoch back to zero in the lifetime of a boot. */
    if (s->epoch < UINT32_MAX) ++s->epoch;
    s->acknowledged = false;
    s->retry_after_ms = 0;
    return true;
}
bool kim_device_state_due(const kim_device_state_t *s, uint32_t now) {
    return s && s->sampled &&
        (s->retry_after_ms == 0 || (int32_t)(now - s->retry_after_ms) >= 0) &&
        (!s->acknowledged || (uint32_t)(now - s->last_sent_ms) >= KIM_DEVICE_STATE_PERIOD_MS);
}
uint32_t kim_device_state_epoch(const kim_device_state_t *s) { return s ? s->epoch : 0; }
bool kim_device_state_format(const kim_device_state_t *s, uint32_t epoch, char *out, size_t cap) {
    if (!s || !s->sampled || !epoch || epoch != s->epoch || !out || !cap) return false;
    int n = snprintf(out, cap, "{\"version\":1,\"epoch\":%lu,\"muted\":%s,\"menu_open\":%s}",
                     (unsigned long)epoch, s->muted ? "true" : "false", s->menu_open ? "true" : "false");
    return n > 0 && (size_t)n < cap;
}
void kim_device_state_sent(kim_device_state_t *s, uint32_t now, uint32_t epoch) {
    if (!s || s->epoch != epoch) return;
    s->acknowledged = true;
    s->last_sent_ms = now;
    s->retry_after_ms = 0;
}
void kim_device_state_failed(kim_device_state_t *s, uint32_t now) {
    if (!s) return;
    s->acknowledged = false;
    s->retry_after_ms = now + 250;
}
