#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include "playback_clock.h"

typedef void *esp_codec_dev_handle_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
#define ESP_CODEC_DEV_OK 0
#define BSP_BUTTON_MUTE_IO 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define KIM_OFFLINE 0
#define KIM_IDLE 1
#define KIM_LISTENING 2
#define KIM_THINKING 3
#define KIM_SPEAKING 4
typedef struct { atomic_uint epoch; } kim_touch_ptt_t;
typedef struct { int unused; } kim_trial_t;
extern int64_t test_now;
#ifdef KIM_VOICE_WORKER_TEST
extern atomic_bool test_cancel;
#else
extern bool test_cancel;
#endif
extern int test_writes;
extern int test_fail_write;
extern void (*test_during_write)(void);
#ifdef KIM_VOICE_WORKER_TEST
static inline int64_t esp_timer_get_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
#else
static inline int64_t esp_timer_get_time(void) { return test_now; }
#endif
static inline int gpio_get_level(int pin) { (void)pin; return test_cancel ? 0 : 1; }
static inline bool kim_touch_cancelled(kim_touch_ptt_t *ptt, unsigned turn) { (void)ptt; (void)turn; return test_cancel; }
static inline int esp_codec_dev_open(esp_codec_dev_handle_t dev, esp_codec_dev_sample_info_t *fs) { (void)dev; (void)fs; return 0; }
static inline int esp_codec_dev_close(esp_codec_dev_handle_t dev) { (void)dev; return 0; }
static inline int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t dev, int vol) { (void)dev; (void)vol; return 0; }
static inline int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t dev, bool mute) { (void)dev; (void)mute; return 0; }
static inline int kim_settings_volume(void) { return 50; }
static inline int esp_codec_dev_write(esp_codec_dev_handle_t dev, void *pcm, int len) {
    (void)dev; (void)pcm; (void)len;
    ++test_writes;
    if (test_during_write) test_during_write();
    return test_writes == test_fail_write ? -1 : ESP_CODEC_DEV_OK;
}
#define pdMS_TO_TICKS(ms) (ms)
#ifdef KIM_VOICE_WORKER_TEST
static inline void vTaskDelay(int ticks) { struct timespec d={ticks/1000, (ticks%1000)*1000000L}; nanosleep(&d,NULL); }
#else
static inline void vTaskDelay(int ticks) { test_now += ticks * 1000; }
#endif
static inline void box_3_microphone_poll(void) {}
