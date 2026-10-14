#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct { unsigned noise_floor, active_frames, silence_frames; bool speech; } kim_vad_t;
void kim_vad_reset(kim_vad_t *v);
bool kim_vad_feed(kim_vad_t *v, const int16_t *samples, size_t count);
