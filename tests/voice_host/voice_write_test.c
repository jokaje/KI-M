/* Compile production voice.c and playback_clock.c with only hardware/network shims. */
#define KIM_VOICE_HOST_TEST 1
#include "../../firmware/main/voice.c"
#include <assert.h>
int64_t test_now;
bool test_cancel;
int test_writes, test_fail_write;
void (*test_during_write)(void);
static uint8_t pcm[1280];
static int poll_state(void) { return kim_voice_display_state(); }
static void blocked_poll(void) {
    test_now += 1000000; /* codec/I2S write may block for 1000 ms */
    assert(poll_state() == KIM_THINKING);
    assert(kim_voice_display_mouth() == 0);
}
static void queued_poll(void) {
    test_now += 10000;
    if (test_writes > 1) assert(poll_state() == KIM_SPEAKING); /* prior successful write */
    else assert(poll_state() == KIM_THINKING);
}
static void abort_poll(void) {
    assert(poll_state() == KIM_THINKING);
    test_cancel = true;
}
static void reset_test(void) {
    test_now = 1000000;
    test_cancel = false;
    test_writes = 0;
    test_fail_write = 0;
    test_during_write = NULL;
    kim_is_online = true;
    kim_voice_state = KIM_THINKING;
    kim_mouth_level = 0;
    kim_reply_failed = false;
    kim_playback_reset(&playback);
    memset(pcm, 0, sizeof(pcm));
    for (size_t i = 0; i < sizeof(pcm); i += 2) {
        pcm[i] = 0x20; pcm[i + 1] = 0x4e; /* RMS 20,000 */
    }
}
static void wav_header(uint8_t *wav, size_t n) {
    memcpy(wav, "RIFF", 4); wav[4] = (uint8_t)(n - 8);
    memcpy(wav + 8, "WAVEfmt ", 8); wav[16] = 16;
    wav[20] = 1; wav[22] = 1; wav[24] = 0x80; wav[25] = 0x3e;
    wav[28] = 0x00; wav[29] = 0x7d; wav[32] = 2; wav[34] = 16;
    memcpy(wav + 36, "data", 4); wav[40] = (uint8_t)(n - 44);
    wav[41] = (uint8_t)((n - 44) >> 8);
    wav[4] = (uint8_t)((n - 8) & 255); wav[5] = (uint8_t)((n - 8) >> 8);
}
int main(void) {
    reset_test();
    stream_audio_t audio = {0};
    test_fail_write = 1; test_during_write = blocked_poll;
    assert(!stream_sink(&audio, pcm, 640));
    assert(test_writes == 1 && poll_state() == KIM_THINKING && !kim_playback_audible(&playback, test_now));
    reset_test(); audio = (stream_audio_t){0};
    test_during_write = blocked_poll;
    assert(stream_sink(&audio, pcm, 640));
    assert(test_writes == 1 && poll_state() == KIM_SPEAKING);
    assert(kim_voice_display_mouth() == 2);
    test_now += 100000;
    assert(poll_state() == KIM_THINKING);
    reset_test(); audio = (stream_audio_t){0};
    assert(stream_sink(&audio, pcm, 640));
    assert(poll_state() == KIM_SPEAKING);
    test_during_write = queued_poll; test_fail_write = 2;
    assert(!stream_sink(&audio, pcm, 640));
    assert(test_writes == 2 && poll_state() == KIM_THINKING);
    reset_test(); audio = (stream_audio_t){0};
    assert(stream_sink(&audio, pcm, 640));
    test_now += 100000;
    assert(poll_state() == KIM_THINKING);
    assert(stream_sink(&audio, pcm, 640));
    assert(poll_state() == KIM_SPEAKING);
    uint8_t wav[44 + sizeof(pcm)] = {0};
    wav_header(wav, sizeof(wav)); memcpy(wav + 44, pcm, sizeof(pcm));
    reset_test(); test_during_write = blocked_poll; test_fail_write = 1;
    assert(!play_reply(wav, sizeof(wav)) && kim_reply_failed);
    assert(test_writes == 1 && poll_state() == KIM_THINKING);
    reset_test(); test_during_write = blocked_poll;
    assert(play_reply(wav, sizeof(wav)));
    assert(test_writes == 2 && poll_state() == KIM_THINKING);
    reset_test(); test_fail_write = 2; test_during_write = queued_poll;
    assert(!play_reply(wav, sizeof(wav)) && kim_reply_failed);
    assert(test_writes == 2 && poll_state() == KIM_THINKING);
    reset_test(); audio = (stream_audio_t){0};
    assert(stream_sink(&audio, pcm, sizeof(pcm)));
    assert(test_writes == 2 && poll_state() == KIM_SPEAKING);
    assert(kim_playback_remaining(&playback, test_now) <= KIM_PLAYBACK_DMA_US);
    reset_test(); audio = (stream_audio_t){0};
    test_during_write = abort_poll;
    assert(!stream_sink(&audio, pcm, sizeof(pcm)));
    assert(test_writes == 1 && poll_state() == KIM_THINKING);
    reset_test(); test_cancel = true;
    assert(!play_reply(wav, sizeof(wav)) && test_writes == 0);
    reset_test(); test_during_write = abort_poll;
    assert(!play_reply(wav, sizeof(wav)) && test_writes == 1);
    assert(poll_state() == KIM_THINKING && !kim_playback_audible(&playback, test_now));
    reset_test(); audio = (stream_audio_t){0};
    test_cancel = true;
    assert(!stream_sink(&audio, pcm, 640) && test_writes == 0);
    puts("voice.c stream/WAV write blocking, failure, success, drain, gap and cancellation: PASS");
    return 0;
}
