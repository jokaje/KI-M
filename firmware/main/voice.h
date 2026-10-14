#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "touch_ptt.h"
extern kim_touch_ptt_t kim_touch_ptt;
extern atomic_bool kim_audio_ready;
extern atomic_int kim_voice_state;
extern atomic_uint kim_mouth_level;
int kim_voice_display_state(void);
unsigned kim_voice_display_mouth(void);
unsigned kim_voice_display_emotion(void);
extern atomic_bool kim_is_online;
extern atomic_bool kim_reply_failed;
extern atomic_bool kim_backend_healthy;
void kim_voice_task(void *arg);
void kim_voice_poll_mute(void);

