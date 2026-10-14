#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* STREAM_PROTOCOL.md v1 + opt-in emotion-v1: M, length=1, payload=1 (happy).
 * Legacy servers send only A/E/X; old firmware never advertises M. */
typedef struct {
    uint8_t header[5];
    size_t header_used, remaining, total_bytes;
    unsigned frames;
    uint8_t pending;
    bool has_pending, ended, failed, metadata_pending;
    unsigned emotion;
} kim_pcm_stream_t;
typedef bool (*kim_pcm_sink_t)(void *ctx, const uint8_t *samples, size_t bytes);
void kim_pcm_begin(kim_pcm_stream_t *s);
bool kim_pcm_feed(kim_pcm_stream_t *s, const uint8_t *data, size_t bytes,
                  kim_pcm_sink_t sink, void *ctx);
bool kim_pcm_finish(const kim_pcm_stream_t *s);
