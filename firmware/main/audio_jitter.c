#include "audio_jitter.h"
#include <string.h>
void kim_jitter_reset(kim_jitter_t *j) { memset(j, 0, sizeof(*j)); }
size_t kim_jitter_push(kim_jitter_t *j, const uint8_t *src, size_t n) {
    if ((n & 1) || n > KIM_JITTER_CAPACITY - j->used) return 0;
    size_t first = n < KIM_JITTER_CAPACITY - j->write ? n : KIM_JITTER_CAPACITY - j->write;
    memcpy(j->data + j->write, src, first); memcpy(j->data, src + first, n - first);
    j->write = (j->write + n) % KIM_JITTER_CAPACITY; j->used += n; return n;
}
size_t kim_jitter_pop(kim_jitter_t *j, uint8_t *dst, size_t n) {
    if ((n & 1) || n > j->used) { ++j->underruns; return 0; }
    size_t first = n < KIM_JITTER_CAPACITY - j->read ? n : KIM_JITTER_CAPACITY - j->read;
    memcpy(dst, j->data + j->read, first); memcpy(dst + first, j->data, n - first);
    j->read = (j->read + n) % KIM_JITTER_CAPACITY; j->used -= n; return n;
}
