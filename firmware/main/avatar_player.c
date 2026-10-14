#include "avatar_player.h"
#include <stddef.h>
#include <string.h>
#include "generated/sprites.h"

unsigned kim_avatar_select_clip(int state, int emotion, int sleep_phase) {
    if (sleep_phase || state == KIM_OFFLINE || state == KIM_SLEEP) return 6;
    if (state == KIM_LISTENING) return 1;
    if (state == KIM_THINKING) return 2;
    if (state == KIM_SPEAKING) return 3;
    if (state == KIM_IDLE && emotion == 1) return 4;
    if (state == KIM_IDLE && emotion == 2) return 5;
    return 0;
}

uint32_t kim_avatar_elapsed(unsigned clip, int sleep_phase, uint32_t sleep_elapsed,
                            uint32_t now_ms, uint32_t clip_started) {
    if (sleep_phase == 2) return 4000 + (sleep_elapsed > 4000 ? 4000 : sleep_elapsed);
    if (sleep_phase == 1) return sleep_elapsed > 4000 ? 4000 : sleep_elapsed;
    return clip == 6 ? 4000 : now_ms - clip_started;
}

bool kim_avatar_frame_valid(unsigned index, uint32_t pack_size) {
    if (index >= KIM_AVATAR_FRAME_COUNT || pack_size != KIM_AVATAR_BYTES) return false;
    kim_avatar_frame_t f = kim_avatar_frames[index];
    return f.length >= 64 && f.offset <= pack_size && f.length <= pack_size - f.offset;
}

void kim_avatar_request(kim_avatar_player_t *p, unsigned clip, uint32_t elapsed_ms, uint32_t epoch) {
    if (!p || clip >= KIM_AVATAR_CLIPS) return;
    kim_avatar_clip_t c = kim_avatar_clips[clip];
    if (!c.count || !c.duration_ms) return;
    unsigned frame = ((uint64_t)(elapsed_ms % c.duration_ms) * c.count) / c.duration_ms;
    atomic_store(&p->desired, c.start + frame);
    atomic_store(&p->desired_epoch, epoch);
}

/* The requested frame may advance while decoding. Accept a completed frame
 * from this clip/generation, but never one from another clip or state epoch. */
static bool same_requested_clip(unsigned ready, unsigned desired) {
    for (unsigned c = 0; c < KIM_AVATAR_CLIPS; ++c) {
        unsigned start = kim_avatar_clips[c].start, end = start + kim_avatar_clips[c].count;
        if (desired >= start && desired < end) return ready >= start && ready < end;
    }
    return false;
}

bool kim_avatar_present(kim_avatar_player_t *p) {
    if (!p || atomic_load(&p->phase) != 2) return false;
    if (!same_requested_clip(atomic_load(&p->ready), atomic_load(&p->desired)) ||
        atomic_load(&p->ready_epoch) != atomic_load(&p->desired_epoch)) {
        atomic_fetch_add(&p->discarded, 1);
        atomic_store(&p->phase, 0); /* discarded back buffer remains worker-owned */
        return false;
    }
    uint16_t *tmp = p->front;
    p->front = p->back;
    p->back = tmp;
    atomic_store(&p->shown, atomic_load(&p->ready));
    atomic_store(&p->shown_epoch, atomic_load(&p->ready_epoch));
    atomic_store(&p->shown_valid, true);
    atomic_fetch_add(&p->presented, 1);
    return true;
}
const uint16_t *kim_avatar_display_data(const kim_avatar_player_t *p, const uint16_t *fallback) {
    if (!p) return fallback;
    /* The front is LVGL-owned even across epochs. A new decode writes only
     * back; present swaps them and release happens after lv_image_set_src. */
    if (atomic_load(&p->shown_valid)) return p->front;
    return fallback;
}
void kim_avatar_release(kim_avatar_player_t *p) {
    if (p && atomic_load(&p->phase) == 2) atomic_store(&p->phase, 0);
}
bool kim_avatar_begin_decode(kim_avatar_player_t *p, uint32_t pack_size, unsigned *index, unsigned *epoch) {
    if (!p || !index || !epoch) return false;
    unsigned wanted = atomic_load(&p->desired), generation = atomic_load(&p->desired_epoch);
    if (!kim_avatar_frame_valid(wanted, pack_size) ||
        (atomic_load(&p->shown_valid) && atomic_load(&p->shown) == wanted &&
         atomic_load(&p->shown_epoch) == generation)) return false;
    unsigned free_phase = 0;
    if (!atomic_compare_exchange_strong(&p->phase, &free_phase, 1)) return false;
    *index = atomic_load(&p->desired);
    *epoch = atomic_load(&p->desired_epoch);
    if (!kim_avatar_frame_valid(*index, pack_size)) {
        atomic_store(&p->phase, 0);
        return false;
    }
    return true;
}
void kim_avatar_finish_decode(kim_avatar_player_t *p, unsigned index, unsigned epoch, bool success) {
    if (!p || atomic_load(&p->phase) != 1) return;
    /* A finished JPEG from an obsolete avatar state must not occupy the
     * backbuffer until the UI happens to reject it. Release it immediately. */
    if (success && epoch != atomic_load(&p->desired_epoch)) {
        atomic_fetch_add(&p->discarded, 1);
        atomic_store(&p->phase, 0);
        return;
    }
    if (success) {
        atomic_fetch_add(&p->decoded, 1);
        atomic_store(&p->ready, index);
        atomic_store(&p->ready_epoch, epoch);
    } else atomic_fetch_add(&p->failed, 1);
    atomic_store(&p->phase, success ? 2 : 0);
}

/* ROM TJPGD output is tightly packed RGB888 for each rectangular tile.
 * Advance the source pointer across rows; destination follows screen stride. */
bool kim_avatar_copy_rgb_rect(uint16_t *pixels, const uint8_t *rgb,
                              unsigned left, unsigned top, unsigned right, unsigned bottom) {
    if (!pixels || !rgb || right >= KIM_AVATAR_SIZE || bottom >= KIM_AVATAR_SIZE ||
        left > right || top > bottom) return false;
    const uint8_t *src = rgb;
    const unsigned width = right - left + 1;
    for (unsigned y = top; y <= bottom; ++y) {
        uint16_t *dst = pixels + (size_t)y * KIM_AVATAR_SIZE + left;
        for (unsigned x = 0; x < width; ++x) {
            const unsigned red = src[0], green = src[1], blue = src[2];
            dst[x] = (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
            src += 3;
        }
    }
    return true;
}

#ifndef KIM_AVATAR_HOST_TEST
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp32s3/rom/tjpgd.h"
extern const uint8_t avatar_start[] asm("_binary_avatar_bin_start");
extern const uint8_t avatar_end[] asm("_binary_avatar_bin_end");
#define PIXELS (KIM_AVATAR_SIZE * KIM_AVATAR_SIZE)
#define WORK_BYTES 8192

typedef struct {
    const uint8_t *data;
    uint32_t size, pos;
    uint16_t *dst;
} decode_t;
static UINT input(JDEC *j, BYTE *dst, UINT count) {
    decode_t *d = j->device;
    UINT n = count < d->size - d->pos ? count : d->size - d->pos;
    if (dst) memcpy(dst, d->data + d->pos, n);
    d->pos += n;
    return n;
}
static UINT output(JDEC *j, void *rgb, JRECT *r) {
    decode_t *d = j->device;
    return kim_avatar_copy_rgb_rect(d->dst, rgb, r->left, r->top, r->right, r->bottom) ? 1 : 0;
}
void kim_avatar_worker(void *arg) {
    kim_avatar_player_t *p = arg;
    uint8_t *work = heap_caps_malloc(WORK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work || (size_t)(avatar_end - avatar_start) != KIM_AVATAR_BYTES) {
        ESP_LOGE("kim-avatar", "work allocation or pack mismatch");
        if (work) heap_caps_free(work);
        vTaskDelete(NULL);
        return;
    }
    unsigned failed_index = UINT32_MAX, failed_epoch = UINT32_MAX;
    unsigned failures = 0;
    for (;;) {
        unsigned index, epoch;
        if (!kim_avatar_begin_decode(p, (uint32_t)(avatar_end - avatar_start), &index, &epoch)) {
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }
        kim_avatar_frame_t frame = kim_avatar_frames[index];
        int64_t decode_started = esp_timer_get_time();
        decode_t d = {.data=avatar_start + frame.offset, .size=frame.length, .dst=p->back};
        JDEC j;
        JRESULT err = jd_prepare(&j, input, work, WORK_BYTES, &d);
        if (err == JDR_OK && j.width == KIM_AVATAR_SIZE && j.height == KIM_AVATAR_SIZE)
            err = jd_decomp(&j, output, 0);
        else if (err == JDR_OK) err = JDR_FMT1;
        unsigned decode_us = (unsigned)(esp_timer_get_time() - decode_started);
        atomic_fetch_add(&p->decode_us_total, decode_us);
        atomic_fetch_add(&p->decode_samples, 1);
        unsigned maximum = atomic_load(&p->decode_us_max);
        while (decode_us > maximum && !atomic_compare_exchange_weak(&p->decode_us_max, &maximum, decode_us)) {}
        if (err == JDR_OK) {
            kim_avatar_finish_decode(p, index, epoch, true);
        } else {
            if (index != failed_index || epoch != failed_epoch) failures = 0;
            failed_index = index;
            failed_epoch = epoch;
            if (failures < 3) ESP_LOGW("kim-avatar", "JPEG decode rejected frame=%u err=%d attempt=%u", index, err, failures + 1);
            else if (failures == 3) ESP_LOGW("kim-avatar", "JPEG decode repeats; suppressing further diagnostics until request changes");
            if (failures < 4) ++failures;
            kim_avatar_finish_decode(p, index, epoch, false);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        taskYIELD();
    }
}
#endif
