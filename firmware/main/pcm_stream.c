#include "pcm_stream.h"
#include <string.h>
#define MAX_FRAME 256000u
#define MAX_TOTAL 1280000u
#define MAX_FRAMES 16u

void kim_pcm_begin(kim_pcm_stream_t *s) { memset(s, 0, sizeof(*s)); }
bool kim_pcm_feed(kim_pcm_stream_t *s, const uint8_t *data, size_t bytes,
                  kim_pcm_sink_t sink, void *ctx) {
    if (!s || !sink || (bytes && !data) || s->failed || (s->ended && bytes)) return false;
    while (bytes) {
        if (s->header_used < 5) {
            size_t n = 5 - s->header_used;
            if (n > bytes) n = bytes;
            memcpy(s->header + s->header_used, data, n);
            s->header_used += n; data += n; bytes -= n;
            if (s->header_used != 5) break;
            uint32_t length = (uint32_t)s->header[1] | ((uint32_t)s->header[2] << 8) |
                              ((uint32_t)s->header[3] << 16) | ((uint32_t)s->header[4] << 24);
            if (s->header[0] == 'A') {
                if (!length || (length & 1) || length > MAX_FRAME || s->frames >= MAX_FRAMES ||
                    length > MAX_TOTAL - s->total_bytes) goto bad;
                s->remaining = length;
                s->frames++;
                s->total_bytes += length;
            } else if (s->header[0] == 'M' && length == 1 && s->frames == 0 && !s->emotion) {
                s->remaining = 1;
                s->metadata_pending = true;
            } else if ((s->header[0] == 'E' || s->header[0] == 'X') && length == 0) {
                s->ended = true;
                s->failed = s->header[0] == 'X';
                if (bytes || s->failed) return false;
                break;
            } else goto bad;
        }
        if (s->metadata_pending) {
            if (!bytes) break;
            if (*data != 1) goto bad;
            s->emotion = *data++;
            --bytes;
            s->remaining = 0;
            s->metadata_pending = false;
            s->header_used = 0;
            continue;
        }
        size_t n = bytes < s->remaining ? bytes : s->remaining;
        if (s->has_pending && n) {
            uint8_t pair[2] = {s->pending, *data++};
            --n; --bytes; --s->remaining;
            s->has_pending = false;
            if (!sink(ctx, pair, 2)) goto bad;
        }
        size_t aligned = n & ~(size_t)1;
        if (aligned && !sink(ctx, data, aligned)) goto bad;
        data += aligned; bytes -= aligned; s->remaining -= aligned;
        if (n != aligned) {
            s->pending = *data++;
            --bytes; --s->remaining;
            s->has_pending = true;
        }
        if (!s->remaining) {
            if (s->has_pending) goto bad;
            s->header_used = 0;
        }
    }
    return true;
bad:
    s->failed = true;
    return false;
}
bool kim_pcm_finish(const kim_pcm_stream_t *s) {
    return s && s->ended && !s->failed && s->frames > 0 && s->header_used == 5 &&
           !s->remaining && !s->has_pending && !s->metadata_pending;
}
