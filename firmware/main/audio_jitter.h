#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Single producer/single consumer, externally serialized; 16kHz mono s16le. */
#define KIM_JITTER_CAPACITY 16384u /* 512 ms */
typedef struct { uint8_t data[KIM_JITTER_CAPACITY]; size_t read, write, used; unsigned underruns; } kim_jitter_t;
void kim_jitter_reset(kim_jitter_t *j);
size_t kim_jitter_push(kim_jitter_t *j, const uint8_t *src, size_t n);
size_t kim_jitter_pop(kim_jitter_t *j, uint8_t *dst, size_t n);
