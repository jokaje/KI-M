#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

/* LVGL owns event writes; voice task consumes snapshots. Epoch invalidates a
 * network/speaker turn on a fresh press, never on ordinary release. */
typedef struct {
    atomic_uint epoch;
    atomic_uint lost_epoch;
    atomic_bool held;
    /* Written only by the LVGL task or while holding its display lock. */
    enum { KIM_PTT_NONE, KIM_PTT_TOUCH, KIM_PTT_BOOT } owner;
    bool boot_raw_low;
    bool boot_down;
    bool boot_armed;
    bool boot_sampled;
    uint32_t boot_changed_ms;
} kim_touch_ptt_t;

static inline void kim_ptt_press(kim_touch_ptt_t *p, int source) {
    if (p->owner != KIM_PTT_NONE) return;
    p->owner = source;
    /* Epoch is the final publication: a reader of the new turn must see held. */
    atomic_store(&p->held, true);
    atomic_fetch_add(&p->epoch, 1);
}
static inline void kim_touch_press(kim_touch_ptt_t *p) {
    kim_ptt_press(p, KIM_PTT_TOUCH);
}
static inline void kim_touch_release(kim_touch_ptt_t *p) {
    if (p->owner != KIM_PTT_TOUCH) return;
    atomic_store(&p->held, false);
    p->owner = KIM_PTT_NONE;
}
static inline void kim_touch_lost(kim_touch_ptt_t *p) {
    if (p->owner != KIM_PTT_TOUCH) return;
    atomic_store(&p->lost_epoch, atomic_load(&p->epoch));
    atomic_store(&p->held, false);
    p->owner = KIM_PTT_NONE;
}
static inline void kim_ptt_cancel(kim_touch_ptt_t *p) {
    /* Even a released input may own the in-flight network/speaker reply. */
    atomic_store(&p->lost_epoch, atomic_load(&p->epoch));
    atomic_store(&p->held, false);
    p->owner = KIM_PTT_NONE;
}
/* Call under the same LVGL lock as touch callbacks, at least every ~10 ms.
 * Startup is disarmed even if the first sample is high: a stable release
 * must precede the first accepted low edge. Unsigned subtraction wraps. */
static inline void kim_boot_sample(kim_touch_ptt_t *p, bool low, uint32_t now_ms) {
    if (!p->boot_sampled || low != p->boot_raw_low) {
        p->boot_sampled = true;
        p->boot_raw_low = low;
        p->boot_changed_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - p->boot_changed_ms) < 30 || p->boot_down == low) {
        if (!low && !p->boot_armed &&
            (uint32_t)(now_ms - p->boot_changed_ms) >= 30) p->boot_armed = true;
        return;
    }
    p->boot_down = low;
    if (!low) {
        p->boot_armed = true;
        if (p->owner == KIM_PTT_BOOT) {
            atomic_store(&p->held, false);
            p->owner = KIM_PTT_NONE;
        }
    } else if (p->boot_armed) {
        kim_ptt_press(p, KIM_PTT_BOOT);
    }
}
static inline bool kim_touch_cancelled(const kim_touch_ptt_t *p, unsigned turn) {
    return atomic_load(&p->epoch) != turn || atomic_load(&p->lost_epoch) == turn;
}
static inline bool kim_touch_recording(const kim_touch_ptt_t *p, unsigned turn) {
    return !kim_touch_cancelled(p, turn) && atomic_load(&p->held);
}
static inline bool kim_touch_submit(const kim_touch_ptt_t *p, unsigned turn,
                                     size_t bytes, bool muted, bool online) {
    return bytes >= 3200 && !muted && online &&
           !kim_touch_cancelled(p, turn) && !atomic_load(&p->held);
}
