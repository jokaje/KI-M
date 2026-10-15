/* Private KI-M PTT: raw s16 mono 16 kHz, bounded in PSRAM, TLS authenticated. */
#if defined(KIM_VOICE_HOST_TEST) || defined(KIM_VOICE_WORKER_TEST)
#include "../../tests/voice_host/voice_shim.h"
#ifdef KIM_VOICE_WORKER_TEST
#include "../../tests/voice_host/worker_shim.h"
#endif
#else
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include "bsp/esp-bsp.h"
#include "box_3_microphone.h"
#include "voice.h"
#include "kim_trial.h"
#include "pcm_stream.h"
#include "playback_clock.h"
#include "audio_jitter.h"
#include "voice_activity.h"
#include "handsfree.h"
#include "generated/sprites.h"
#include "esp_codec_dev.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "kim_runtime.h"
#include "kim_character_state.h"
#include "kim_device_state.h"
#include "kim_menu.h"
#include "kim_settings.h"
#include "kim_wifi.h"
#include "esp_netif_sntp.h"
#endif
#if defined(KIM_VOICE_HOST_TEST) || defined(KIM_VOICE_WORKER_TEST)
#include "../../tests/voice_host/config_test.h"
#elif __has_include("config_local.h")
#include "config_local.h"
#else
#include "config_local.example.h"
#endif

#define RATE 16000

/* Optional realtime hands-free listening. Default OFF: nothing changes
 * until this is explicitly enabled on the bench. NO AEC: while a reply
 * is audible, hands-free capture pauses (half-duplex by construction).
 * PTT keeps working exactly as before. */
#ifndef KIM_ENABLE_HANDSFREE
#define KIM_ENABLE_HANDSFREE 0
#endif

#define HANDSFREE_CAPTURE (RATE * 2 * 12) /* 12 s fits an 8 s speech cap plus context */
#define MAX_CAPTURE (RATE * 2 * 8)
#define MAX_REPLY (RATE * 2 * 40 + 44)
static const char *TAG = "kim-voice";
atomic_int kim_voice_state = KIM_OFFLINE;
atomic_uint kim_mouth_level;
atomic_bool kim_is_online;
atomic_bool kim_reply_failed;
kim_touch_ptt_t kim_touch_ptt;
atomic_bool kim_audio_ready;
atomic_bool kim_backend_healthy;
static unsigned active_turn;
static esp_codec_dev_handle_t mic, speaker;
static kim_playback_clock_t playback;
static atomic_int_fast64_t emotion_until_us;
unsigned kim_voice_display_emotion(void) {
    return atomic_load(&emotion_until_us) > esp_timer_get_time() ? 1 : 0;
}
static bool cancelled(void);
int kim_voice_display_state(void) {
    int state = atomic_load(&kim_voice_state);
    if (state != KIM_THINKING) return state;
    int64_t now = esp_timer_get_time();
    if (kim_playback_audible(&playback, now)) return KIM_SPEAKING;
    return KIM_THINKING;
}
unsigned kim_voice_display_mouth(void) {
    return kim_playback_level(&playback, esp_timer_get_time());
}
static bool drain_playback(void) {
    while (kim_playback_remaining(&playback, esp_timer_get_time()) > 0) {
        if (cancelled() || !kim_is_online) {
            kim_playback_reset(&playback);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    kim_playback_reset(&playback);

    return true;
}
static uint8_t *capture;
extern kim_trial_t kim_ota_trial;
static bool capture_nonzero(const uint8_t *pcm, size_t size) {
    for (size_t i = 0; i < size; ++i) if (pcm[i]) return true;
    return false;
}
static bool cancelled(void) {
    return gpio_get_level(BSP_BUTTON_MUTE_IO) == 0 ||
           kim_touch_cancelled(&kim_touch_ptt, active_turn);
}
void kim_voice_poll_mute(void) { box_3_microphone_poll(); }

/* The Wi-Fi worker owns all radio operations; voice consumes its IP state. */
static bool tls_time_ready(void) {
    time_t now = time(NULL);
    return now >= 1700000000;
}

/* Parse canonical 44-byte little-endian PCM WAV. Never play unknown codecs or oversize reply. */
static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }
static bool valid_wav(const uint8_t *h, int total) {
    return total >= 44 && total <= MAX_REPLY && memcmp(h, "RIFF", 4) == 0 &&
           memcmp(h + 8, "WAVEfmt ", 8) == 0 && le32(h + 16) == 16 &&
           le16(h + 20) == 1 && le16(h + 22) == 1 && le32(h + 24) == RATE &&
           le32(h + 28) == RATE * 2 && le16(h + 32) == 2 && le16(h + 34) == 16 &&
           memcmp(h + 36, "data", 4) == 0 && le32(h + 40) == (uint32_t)(total - 44) &&
           le32(h + 4) == (uint32_t)(total - 8) && (total - 44) % 2 == 0;
}

/* esp_codec_dev_new zero-initializes volume to 0 (-96 dB); open reapplies
 * that cached setting. Explicitly configure each opening before Speaking. */
static bool open_speaker(void) {
    esp_codec_dev_sample_info_t fs = {.sample_rate = RATE, .channel = 1, .bits_per_sample = 16};
    int result = esp_codec_dev_open(speaker, &fs);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "speaker open failed code=%d", result);
        return false;
    }
    int volume_result = esp_codec_dev_set_out_vol(speaker, kim_settings_volume());
    int mute_result = volume_result == ESP_CODEC_DEV_OK ?
        esp_codec_dev_set_out_mute(speaker, false) : volume_result;
    if (volume_result != ESP_CODEC_DEV_OK || mute_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "speaker configure failed volume=%d mute=%d", volume_result, mute_result);
        esp_codec_dev_close(speaker);
        return false;
    }
    ESP_LOGI(TAG, "speaker configured volume=%d mute=off", kim_settings_volume());
    return true;
}

static bool play_reply(uint8_t *wav, int len) {
    if (!valid_wav(wav, len) || cancelled() || !capture_nonzero(wav + 44, len - 44)) return false;
    if (!open_speaker()) { kim_reply_failed = true; return false; }
    kim_playback_reset(&playback);
    bool played = true;
    for (int pos = 44; pos < len && !cancelled() && kim_is_online; pos += 640) {
        int size = len - pos < 640 ? len - pos : 640;
        unsigned mouth = kim_playback_rms_level(wav + pos, size);
        if (esp_codec_dev_write(speaker, wav + pos, size) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "speaker WAV write failed");
            kim_reply_failed = true;
            played = false;
            break;
        }
        kim_playback_written(&playback, esp_timer_get_time(), size, mouth);
    }
    kim_mouth_level = 0;
    if (played && !cancelled() && kim_is_online) played = drain_playback();
    else kim_playback_reset(&playback);
    esp_codec_dev_close(speaker);
    return played && !cancelled() && kim_is_online;
}

/* Only 404/405/501 from an otherwise authenticated server enable legacy fallback. */
typedef enum { STREAM_OK, STREAM_UNSUPPORTED, STREAM_FAILED } stream_result_t;
#ifndef KIM_VOICE_HOST_TEST
typedef struct { bool content_type_ok; bool transfer_encoding; bool content_length; } stream_headers_t;
#endif
typedef struct { bool opened, nonzero; } stream_audio_t;
#ifndef KIM_VOICE_HOST_TEST
static esp_err_t stream_event(esp_http_client_event_t *event) {
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        stream_headers_t *h = event->user_data;
        if (!strcasecmp(event->header_key, "Content-Type"))
            h->content_type_ok = !strcmp(event->header_value, "application/vnd.kim.stream-v1");
        if (!strcasecmp(event->header_key, "Transfer-Encoding")) h->transfer_encoding = true;
        if (!strcasecmp(event->header_key, "Content-Length")) h->content_length = true;
    }
    return ESP_OK;
}
#endif
#ifndef KIM_VOICE_HOST_TEST
/* A single-producer/single-consumer PCM queue decouples HTTP reads from I2S.
 * The playback task owns all stream speaker writes. The mutex protects FIFO
 * indices; it is NEVER held across a blocking codec write. */
typedef struct {
    kim_jitter_t fifo;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    atomic_bool done, stop, failed, exited;
    bool started;
    unsigned underruns;
} stream_player_t;
static stream_player_t *stream_player;
static int64_t stream_deadline;
static void stream_playback_task(void *arg) {
    stream_player_t *p = arg;
    uint8_t frame[640];
    bool primed = false;
    bool starvation_reported = false;
    while (!atomic_load(&p->stop)) {
        size_t n = 0;
        xSemaphoreTake(p->lock, portMAX_DELAY);
        size_t available = p->fifo.used;
        /* Prebuffer 60 ms on startup, but don't deadlock short responses. */
        /* Require 60 ms after startup or starvation. Do not block the tail. */
        if (!primed && (available >= 1920 || atomic_load(&p->done))) {
            primed = true;
            starvation_reported = false;
        }
        if (primed && available >= sizeof(frame)) n = kim_jitter_pop(&p->fifo, frame, sizeof(frame));
        else if (primed && atomic_load(&p->done) && available) n = kim_jitter_pop(&p->fifo, frame, available);
        bool finished = atomic_load(&p->done) && p->fifo.used == 0;
        xSemaphoreGive(p->lock);
        if (n) {
            unsigned mouth = kim_playback_rms_level(frame, n);
            if (esp_codec_dev_write(speaker, frame, n) != ESP_CODEC_DEV_OK) {
                atomic_store(&p->failed, true);
                kim_playback_reset(&playback);
                break;
            }
            int64_t now = esp_timer_get_time();
            kim_playback_written(&playback, now, n, mouth);
        } else if (finished) break;
        else {
            /* Count starvation EVENTS, not one underrun per 5 ms poll.
             * Re-prime to 60 ms before resuming to avoid repeated pops. */
            if (primed && !atomic_load(&p->done) && available < sizeof(frame)) {
                if (!starvation_reported) {
                    ++p->underruns;
                    starvation_reported = true;
                }
                primed = false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    /* Final p access: owner reclaims resources only after completion. */
    atomic_store_explicit(&p->exited, true, memory_order_release);
    vTaskDelete(NULL);
}
static bool stream_player_start(stream_player_t *p) {
    memset(p, 0, sizeof(*p));
    kim_jitter_reset(&p->fifo);
    p->lock = xSemaphoreCreateMutex();
    if (!p->lock) return false;
    /* Audio owns core 1: Wi-Fi/LWIP and the IDLE0 watchdog stay unperturbed on
     * core 0 even while the codec write blocks. */
    if (xTaskCreatePinnedToCore(stream_playback_task, "kim-playback", 4096, p, 5, &p->task, 1) != pdPASS) {
        vSemaphoreDelete(p->lock);
        p->lock = NULL;
        return false;
    }
    p->started = true;
    return true;
}
static bool stream_player_push(stream_player_t *p, const uint8_t *data, size_t len) {
    while (len && !atomic_load(&p->failed) && !atomic_load(&p->stop) &&
           !cancelled() && kim_is_online && esp_timer_get_time() <= stream_deadline) {
        xSemaphoreTake(p->lock, portMAX_DELAY);
        size_t room = KIM_JITTER_CAPACITY - p->fifo.used;
        size_t n = len < room ? len : room;
        n &= ~(size_t)1;
        if (n) kim_jitter_push(&p->fifo, data, n);
        xSemaphoreGive(p->lock);
        if (!n) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        data += n; len -= n;
    }
    return len == 0;
}
static bool stream_player_finish(stream_player_t *p, bool success) {
    if (!p->started) return success;
    atomic_store(&p->done, true);
    if (!success) atomic_store(&p->stop, true);
    /* Pinned IDF6 adapter uses a 1000 ms I2S write timeout. Allow complete
     * playback of a <=40 s reply; cancellation needs at most one write. If
     * driver contract fails, reset rather than free an active task/codec. */
    int64_t deadline = esp_timer_get_time() + (success ? 45000000LL : 2500000LL);
    while (!atomic_load_explicit(&p->exited, memory_order_acquire)) {
        if (cancelled() || !kim_is_online || atomic_load(&p->failed)) {
            atomic_store(&p->stop, true);
            int64_t stop_deadline = esp_timer_get_time() + 2500000LL;
            if (stop_deadline < deadline) deadline = stop_deadline;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "playback I2S completion exceeded bound; restarting");
            esp_restart(); /* Do not kill a task holding driver locks. */
            abort();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    bool ok = success && !atomic_load(&p->failed) && !atomic_load(&p->stop);
    ESP_LOGI(TAG, "stream FIFO starvation events=%u", p->underruns);
    vSemaphoreDelete(p->lock);
    p->lock = NULL;
    p->started = false;
    return ok;
}
#endif
static bool stream_sink(void *ctx, const uint8_t *pcm, size_t bytes) {
    stream_audio_t *audio = ctx;
    if (cancelled() || !kim_is_online) return false;
    if (capture_nonzero(pcm, bytes)) audio->nonzero = true;
    if (!audio->opened) {
        if (!open_speaker()) return false;
        audio->opened = true;
    }
#ifndef KIM_VOICE_HOST_TEST
    if (stream_player) return stream_player_push(stream_player, pcm, bytes);
#endif
    for (size_t pos = 0; pos < bytes; ) {
        if (cancelled() || !kim_is_online) {
            kim_playback_reset(&playback);
                    kim_voice_state = KIM_THINKING;
            return false;
        }
        size_t n = bytes - pos < 640 ? bytes - pos : 640;
        unsigned mouth = kim_playback_rms_level(pcm + pos, n);
        if (esp_codec_dev_write(speaker, (void *)(pcm + pos), n) != ESP_CODEC_DEV_OK) {
            kim_playback_reset(&playback);
                    kim_voice_state = KIM_THINKING;
            return false;
        }
        int64_t written_at = esp_timer_get_time();
        kim_playback_written(&playback, written_at, n, mouth);
        pos += n;
    }
    /* Display derives SPEAKING from queued PCM until it drains; never hold
     * SPEAKING across an unbounded network/TTS gap. */
    kim_voice_state = KIM_THINKING;
    kim_mouth_level = 0;
    return true;
}
#ifndef KIM_VOICE_HOST_TEST
static stream_result_t send_stream(size_t len) {
    /* Replace only the documented route suffix. Never silently redirect a custom URL. */
    const char suffix[] = "/v1/turn";
    size_t url_len = strlen(KIM_SERVER_URL);
    if (url_len < sizeof(suffix) - 1 ||
        strcmp(KIM_SERVER_URL + url_len - (sizeof(suffix) - 1), suffix)) return STREAM_UNSUPPORTED;
    char url[256];
    int printed = snprintf(url, sizeof(url), "%.*s/v1/turn-stream",
                           (int)(url_len - (sizeof(suffix) - 1)), KIM_SERVER_URL);
    if (printed < 0 || printed >= sizeof(url)) return STREAM_FAILED;
    stream_headers_t headers = {0};
    stream_audio_t audio = {0};
    stream_player_t *player = heap_caps_calloc(1, sizeof(stream_player_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!player) return STREAM_FAILED;
    stream_player = NULL;
    kim_playback_reset(&playback);
    kim_pcm_stream_t parser;
    kim_pcm_begin(&parser);
    stream_result_t outcome = STREAM_FAILED;
    esp_http_client_config_t cfg = {.url = url, .cert_pem = KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL,
                                     .crt_bundle_attach = KIM_SERVER_CA_PEM[0] ? NULL : esp_crt_bundle_attach,
                                     .timeout_ms = 1000, .buffer_size = 2048,
                                     .event_handler = stream_event, .user_data = &headers};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(player); return outcome; }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/vnd.kim.pcm;rate=16000;channels=1;format=s16le");
    esp_http_client_set_header(client, "X-KIM-Capabilities", "emotion-v1");
    char auth[256];
    if (snprintf(auth, sizeof(auth), "Bearer %s", KIM_DEVICE_TOKEN) >= sizeof(auth)) goto close_stream;
    esp_http_client_set_header(client, "Authorization", auth);
    esp_err_t open_err = esp_http_client_open(client, len);
    if (open_err != ESP_OK) {
        ESP_LOGW(TAG, "stream open err=0x%x", (unsigned)open_err);
        goto close_stream;
    }
    ESP_LOGI(TAG, "stream TLS opened, sending PCM bytes=%u", (unsigned)len);
    for (size_t pos = 0; pos < len && !cancelled() && kim_is_online;) {
        int n = esp_http_client_write(client, (const char *)capture + pos, len - pos < 2048 ? len - pos : 2048);
        if (n <= 0) goto close_stream;
        pos += n;
    }
    if (cancelled() || !kim_is_online) goto close_stream;
    int64_t deadline = esp_timer_get_time() + 160000000LL;
    stream_deadline = deadline;
    int64_t response;
    do {
        if (cancelled() || !kim_is_online || esp_timer_get_time() > deadline) goto close_stream;
        response = esp_http_client_fetch_headers(client);
    } while (response == -ESP_ERR_HTTP_EAGAIN && (vTaskDelay(pdMS_TO_TICKS(2)), true));
    if (response < 0) goto close_stream;
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "stream headers response=%lld status=%d type=%d length=%d encoding=%d",
             (long long)response, status, headers.content_type_ok, headers.content_length, headers.transfer_encoding);
    if (status == 404 || status == 405 || status == 501) {
        outcome = STREAM_UNSUPPORTED;
        goto close_stream;
    }
    if (status != 200 || !headers.content_type_ok || headers.content_length ||
        headers.transfer_encoding) goto close_stream;
    uint8_t buffer[640];
    /* Start playback only after TLS authentication and response validation. */
    if (!stream_player_start(player)) goto close_stream;
    stream_player = player;
    while (!cancelled() && kim_is_online && esp_timer_get_time() <= deadline) {
        int n = esp_http_client_read(client, (char *)buffer, sizeof(buffer));
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        if (n < 0) { ESP_LOGW(TAG, "stream read err=%d", n); break; }
        if (!n) {
            if (kim_pcm_finish(&parser) && audio.opened && audio.nonzero &&
                !cancelled() && kim_is_online) outcome = STREAM_OK;
            ESP_LOGI(TAG, "stream eof terminal=%d frames=%u", outcome == STREAM_OK, parser.frames);
            break; /* FIN without E is an interrupted reply. */
        }
        if (!kim_pcm_feed(&parser, buffer, n, stream_sink, &audio)) {
            ESP_LOGW(TAG, "stream parse/play failed bytes=%d frames=%u", n, parser.frames);
            break;
        }

    }
close_stream:
    stream_player = NULL;
    if (player->started && !stream_player_finish(player, outcome == STREAM_OK)) outcome = STREAM_FAILED;
    if (outcome != STREAM_OK) atomic_store(&emotion_until_us, 0);
    heap_caps_free(player);
    kim_mouth_level = 0;
    if (audio.opened) {
        if (outcome == STREAM_OK && !drain_playback()) outcome = STREAM_FAILED;
        else { kim_playback_reset(&playback);  }
        esp_codec_dev_close(speaker);
    }
    if (outcome == STREAM_OK && parser.emotion == 1 && !cancelled() && kim_is_online)
        atomic_store(&emotion_until_us, esp_timer_get_time() + 2000000LL);
    else atomic_store(&emotion_until_us, 0);
    if (outcome == STREAM_OK) kim_trial_reply(&kim_ota_trial, audio.nonzero, true);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return outcome;
}

#ifndef KIM_VOICE_WORKER_TEST
extern kim_character_poll_t kim_character_poll;
static esp_err_t character_header(esp_http_client_event_t *event) {
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        !strcasecmp(event->header_key, "Content-Type") && event->user_data)
        *(bool *)event->user_data = event->header_value &&
            !strcasecmp(event->header_value, "application/json");
    return ESP_OK;
}
static kim_character_gate_t character_gate(void) {
    return (kim_character_gate_t){
        .online = atomic_load(&kim_is_online),
        .healthy = atomic_load(&kim_backend_healthy),
        .idle = !kim_runtime_voice_busy() && kim_voice_display_state() == KIM_IDLE,
        .released = !atomic_load(&kim_touch_ptt.held),
        .menu_allowed = kim_menu_recording_allowed(),
        .unmuted = gpio_get_level(BSP_BUTTON_MUTE_IO) != 0,
    };
}
/* The metadata task alone owns this snapshot and its small TLS request. Physical
 * GPIO1 is input-only; menu_open reads the UI writer's atomic gate snapshot. */
static kim_device_state_t device_state;
static void device_state_sample(void) {
    kim_device_state_sample(&device_state, (uint32_t)(esp_timer_get_time() / 1000),
                            gpio_get_level(BSP_BUTTON_MUTE_IO) == 0, kim_menu_open());
}
static void device_state_report(void) {
    device_state_sample();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!kim_device_state_due(&device_state, now) || !atomic_load(&kim_is_online)) return;
    /* A busy voice turn may defer this request; it must never block PTT. */
    if (!kim_tls_probe_begin()) return;
    uint32_t epoch = kim_device_state_epoch(&device_state);
    const char suffix[] = "/v1/turn";
    size_t length = strlen(KIM_SERVER_URL);
    char url[256], auth[256], body[KIM_DEVICE_STATE_JSON_CAP];
    bool success = false;
    if (length < sizeof(suffix) - 1 ||
        strcmp(KIM_SERVER_URL + length - sizeof(suffix) + 1, suffix)) goto done_state;
    int url_len = snprintf(url, sizeof(url), "%.*s/v1/device-state",
                           (int)(length - sizeof(suffix) + 1), KIM_SERVER_URL);
    int auth_len = snprintf(auth, sizeof(auth), "Bearer %s", KIM_DEVICE_TOKEN);
    if (url_len < 0 || (size_t)url_len >= sizeof(url) ||
        auth_len < 0 || (size_t)auth_len >= sizeof(auth) ||
        !kim_device_state_format(&device_state, epoch, body, sizeof(body))) goto done_state;
    esp_http_client_config_t cfg = {.url = url,
        .cert_pem = KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL,
        .crt_bundle_attach = KIM_SERVER_CA_PEM[0] ? NULL : esp_crt_bundle_attach,
        .timeout_ms = 300, .buffer_size = 256, .disable_auto_redirect = true};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) goto done_state;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (esp_http_client_set_header(client, "Authorization", auth) == ESP_OK &&
        esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK &&
        esp_http_client_set_post_field(client, body, strlen(body)) == ESP_OK &&
        esp_http_client_perform(client) == ESP_OK &&
        esp_http_client_get_status_code(client) == 204)
        success = true;
    esp_http_client_cleanup(client);
done_state:
    /* A changed physical gate wins over an old successful in-flight report. */
    device_state_sample();
    now = (uint32_t)(esp_timer_get_time() / 1000);
    if (success) kim_device_state_sent(&device_state, now, epoch);
    else kim_device_state_failed(&device_state, now);
    kim_tls_probe_end();
}
/* A separate endpoint, but the same origin, bearer, and verified CA policy
 * as /v1/health. Never parse a partial, redirected or oversized response. */
static void character_probe(void) {
    kim_character_gate_t gate = character_gate();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!kim_character_due(&kim_character_poll, now, &gate) ||
        !kim_tls_probe_begin()) return;
    kim_character_poll_started(&kim_character_poll, now);
    unsigned generation = atomic_load(&kim_character_poll.generation);
    uint32_t privacy_epoch = kim_device_state_epoch(&device_state);
    const char suffix[] = "/v1/turn";
    size_t length = strlen(KIM_SERVER_URL);
    if (length < sizeof(suffix) - 1 ||
        strcmp(KIM_SERVER_URL + length - sizeof(suffix) + 1, suffix)) goto done;
    char url[256], auth[256], body[193];
    int n = snprintf(url, sizeof(url), "%.*s/v1/state", (int)(length - sizeof(suffix) + 1), KIM_SERVER_URL);
    if (n < 0 || n >= sizeof(url) ||
        snprintf(auth, sizeof(auth), "Bearer %s", KIM_DEVICE_TOKEN) >= sizeof(auth)) goto done;
    bool json_type = false;
    esp_http_client_config_t cfg = {.url = url,
        .cert_pem = KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL,
        .crt_bundle_attach = KIM_SERVER_CA_PEM[0] ? NULL : esp_crt_bundle_attach,
        .timeout_ms = 150, .buffer_size = 256,
        .event_handler = character_header, .user_data = &json_type};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) goto done;
    device_state_sample();
    gate = character_gate();
    if (kim_device_state_epoch(&device_state) != privacy_epoch ||
        atomic_load(&kim_character_poll.generation) != generation ||
        !kim_character_eligible(&gate) ||
        (uint32_t)((esp_timer_get_time() / 1000) - now) >= 900) goto close_state;
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    if (esp_http_client_set_header(client, "Authorization", auth) != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK ||
        esp_http_client_open(client, 0) != ESP_OK) goto close_state;
    device_state_sample();
    gate = character_gate();
    if (kim_device_state_epoch(&device_state) != privacy_epoch ||
        atomic_load(&kim_character_poll.generation) != generation ||
        !kim_character_eligible(&gate) ||
        (uint32_t)((esp_timer_get_time() / 1000) - now) >= 900) goto close_state;
    int64_t declared = esp_http_client_fetch_headers(client);
    device_state_sample();
    if ((uint32_t)((esp_timer_get_time() / 1000) - now) >= 900 ||
        kim_device_state_epoch(&device_state) != privacy_epoch ||
        atomic_load(&kim_character_poll.generation) != generation ||
        declared < 1 || declared > 192 || !json_type ||
        esp_http_client_get_status_code(client) != 200 ||
        esp_http_client_is_chunked_response(client)) goto close_state;
    int received = 0;
    while (received < declared &&
           (uint32_t)((esp_timer_get_time() / 1000) - now) < 900 &&
           atomic_load(&kim_character_poll.generation) == generation &&
           kim_device_state_epoch(&device_state) == privacy_epoch &&
           !atomic_load(&kim_touch_ptt.held)) {
        int got = esp_http_client_read(client, body + received, (int)declared - received);
        if (got <= 0) break;
        received += got;
    }
    kim_character_state_t suggested;
    device_state_sample();
    gate = character_gate();
    if ((uint32_t)((esp_timer_get_time() / 1000) - now) < 900 &&
        kim_device_state_epoch(&device_state) == privacy_epoch &&
        atomic_load(&kim_character_poll.generation) == generation &&
        received == declared && esp_http_client_is_complete_data_received(client) &&
        kim_character_parse(body, (size_t)received, &suggested) &&
        kim_character_eligible(&gate))
        kim_character_offer(&kim_character_poll, generation, suggested);
close_state:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
done:
    kim_tls_probe_end();
}
/* Only this low-priority task may issue metadata I/O. It never holds the LVGL
 * writer lock or the voice/maintenance lock, and a press invalidates the epoch
 * even when a TLS call is in progress. The UI is the sole suggestion writer. */
static void character_task(void *arg) {
    (void)arg;
    for (;;) {
        device_state_report();
        if (!kim_device_state_due(&device_state, (uint32_t)(esp_timer_get_time() / 1000)))
            character_probe();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static bool health_probe(void) {
    const char suffix[] = "/v1/turn";
    size_t length = strlen(KIM_SERVER_URL);
    if (length < sizeof(suffix) - 1 || strcmp(KIM_SERVER_URL + length - sizeof(suffix) + 1, suffix))
        return false;
    char url[256], auth[256];
    int n = snprintf(url, sizeof(url), "%.*s/v1/health", (int)(length - sizeof(suffix) + 1), KIM_SERVER_URL);
    if (n < 0 || n >= sizeof(url) ||
        snprintf(auth, sizeof(auth), "Bearer %s", KIM_DEVICE_TOKEN) >= sizeof(auth)) return false;
    esp_http_client_config_t cfg = {.url = url, .cert_pem = KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL,
                                     .crt_bundle_attach = KIM_SERVER_CA_PEM[0] ? NULL : esp_crt_bundle_attach,
                                     .timeout_ms = 5000, .buffer_size = 512};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    ESP_LOGI(TAG, "authenticated nonrecording health: err=0x%x status=%d", (unsigned)err, status);
    if (err != ESP_OK || status != 204)
        ESP_LOGW(TAG, "health failure heap internal free=%u largest=%u psram free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    esp_http_client_cleanup(client);
    return err == ESP_OK && status == 204;
}

#endif /* worker-test excludes health probe */
static void send_turn(size_t len) {
    uint8_t *reply = heap_caps_malloc(MAX_REPLY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) { kim_voice_state = KIM_OFFLINE; return; }
    esp_http_client_config_t cfg = {.url = KIM_SERVER_URL, .cert_pem = KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL,
                                     .crt_bundle_attach = KIM_SERVER_CA_PEM[0] ? NULL : esp_crt_bundle_attach,
                                     .timeout_ms = 1000, .buffer_size = 2048};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) goto done;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/vnd.kim.pcm;rate=16000;channels=1;format=s16le");
    char auth[256];
    if (snprintf(auth, sizeof(auth), "Bearer %s", KIM_DEVICE_TOKEN) >= sizeof(auth)) goto close;
    esp_http_client_set_header(client, "Authorization", auth);
    if (esp_http_client_open(client, len) != ESP_OK) goto close;
    for (size_t pos = 0; pos < len && !cancelled() && kim_is_online;) {
        int n = esp_http_client_write(client, (const char *)capture + pos, len - pos < 2048 ? len - pos : 2048);
        if (n <= 0) goto close;
        pos += n;
    }
    if (cancelled() || !kim_is_online) goto close;
    int64_t deadline = esp_timer_get_time() + 160000000LL;
    int64_t headers;
    do {
        if (cancelled() || !kim_is_online || esp_timer_get_time() > deadline) goto close;
        headers = esp_http_client_fetch_headers(client);
    } while (headers == -ESP_ERR_HTTP_EAGAIN && (vTaskDelay(pdMS_TO_TICKS(2)), true));
    if (headers < 0 || esp_http_client_get_status_code(client) != 200) goto close;
    int expected = esp_http_client_get_content_length(client);
    if (expected < 44 || expected > MAX_REPLY || esp_http_client_is_chunked_response(client)) goto close;
    int received = 0;
    while (received < expected && !cancelled() && kim_is_online) {
        int n = esp_http_client_read(client, (char *)reply + received, expected - received < 2048 ? expected - received : 2048);
        if (n <= 0) break;
        received += n;
    }
    if (received == expected && !cancelled() && kim_is_online && play_reply(reply, received))
        kim_trial_reply(&kim_ota_trial, true, true);
close:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
done:
    memset(reply, 0, MAX_REPLY);
    heap_caps_free(reply);
}

static void send_response(size_t len) {
    atomic_store(&emotion_until_us, 0);
    stream_result_t streamed = send_stream(len);
    if (streamed == STREAM_UNSUPPORTED && !cancelled() && kim_is_online)
        send_turn(len); /* Older authenticated backend, WAV route. */
    else if (streamed == STREAM_FAILED && !cancelled())
        kim_reply_failed = true;
}
#ifndef KIM_VOICE_WORKER_TEST
void kim_voice_task(void *arg) {
    (void)arg;
    /* GPIO1 is status only, never driven; GPIO0 is no longer a voice input. */
    const bool provisioned = KIM_SERVER_URL[0] &&
        strlen(KIM_DEVICE_TOKEN) >= 32 && strncmp(KIM_SERVER_URL, "https://", 8) == 0;
    if (!provisioned) ESP_LOGW(TAG, "Unprovisioned: audio/UI active, voice network disabled");
    capture = heap_caps_malloc(MAX_CAPTURE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!capture) { ESP_LOGE(TAG, "capture allocation failed"); vTaskDelete(NULL); return; }
    /* Speaker init creates shared I2S; power-cycled ADC adapter recreates codec on unmute. */
    ESP_LOGI(TAG, "audio startup: speaker begin");
    /* A transient codec/I2C failure must not permanently kill touch handling. */
    while (!speaker || !mic) {
        if (!speaker) speaker = bsp_audio_codec_speaker_init();
        ESP_LOGI(TAG, "audio startup: speaker=%s microphone begin", speaker ? "ready" : "failed");
        if (speaker && !mic) mic = box_3_microphone_create();
        if (!speaker || !mic) {
            ESP_LOGW(TAG, "audio codec init failed; retrying, GPIO1=%d",
                     gpio_get_level(BSP_BUTTON_MUTE_IO));
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    ESP_LOGI(TAG, "audio startup complete; PTT enabled");
    atomic_store(&kim_audio_ready, true);
    if (provisioned && xTaskCreatePinnedToCore(character_task, "kim-state", 6144,
                                               NULL, 1, NULL, 1) != pdPASS)
        ESP_LOGW(TAG, "metadata poll unavailable");
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    bool sntp_started = esp_netif_sntp_init(&sntp) == ESP_OK;
    bool time_synced = false;
    TickType_t next_health = 0;
    unsigned consumed = atomic_load(&kim_touch_ptt.epoch);
    while (1) {
        if (kim_wifi_connected() && sntp_started && !time_synced &&
            esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK)
            time_synced = tls_time_ready();
        kim_is_online = kim_wifi_connected() && provisioned && time_synced;
        if (!kim_is_online) kim_backend_healthy = false;
        else if ((int32_t)(xTaskGetTickCount() - next_health) >= 0) {
            /* Never present a TLS/backend as healthy until SNTP gives sane time
             * and the authenticated nonrecording request returns 204. */
            if (kim_tls_probe_begin()) {
                kim_backend_healthy = health_probe();
                kim_tls_probe_end();
                next_health = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
            } else {
                /* Resource transition, not a backend failure. Retry without
                 * spinning; BLE ACTIVE with stable pools still permits probes. */
                next_health = xTaskGetTickCount() + pdMS_TO_TICKS(250);
            }
        }
        bool muted = gpio_get_level(BSP_BUTTON_MUTE_IO) == 0;
        unsigned turn = atomic_load(&kim_touch_ptt.epoch);
        kim_voice_state = kim_is_online ? KIM_IDLE : KIM_OFFLINE;
#if KIM_ENABLE_HANDSFREE
        /* Realtime hands-free attempt. Runs only when every 0.5.0 gate is
         * true: PTT never held, mic not muted, online, backend healthy,
         * allowed by menu, no reply playback in any phase, no BLE/TLS
         * resource transition, and the runtime audio path is free. The
         * halbduplex rule is enforced by state: while speaking or
         * thinking/receiving a reply, this block is skipped entirely,
         * so we never sample our own speaker through the mic. */
        static kim_handsfree_t hf;
        static uint8_t *hf_capture;
        if (turn == consumed && !muted &&
            kim_is_online && kim_backend_healthy &&
            kim_menu_recording_allowed() &&
            kim_voice_display_state() == KIM_IDLE &&
            kim_runtime_voice_begin()) {
            if (!hf_capture) {
                hf_capture = heap_caps_malloc(HANDSFREE_CAPTURE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!hf_capture) { kim_runtime_voice_end(); goto wait_input; }
            }
            esp_codec_dev_sample_info_t fs = {.sample_rate = RATE, .channel = 1, .bits_per_sample = 16};
            int open_result = esp_codec_dev_open(mic, &fs);
            if (open_result == ESP_CODEC_DEV_OK) {
                kim_handsfree_init(&hf, hf_capture, HANDSFREE_CAPTURE);
                kim_voice_state = KIM_LISTENING;
                bool mic_ok = true;
                bool complete = false;
                size_t frames = 0;
                while (frames * KIM_HF_FRAME_BYTES < HANDSFREE_CAPTURE &&
                       gpio_get_level(BSP_BUTTON_MUTE_IO) != 0 &&
                       kim_menu_recording_allowed() && kim_wifi_connected() &&
                       kim_is_online && atomic_load(&kim_touch_ptt.epoch) == consumed) {
                    uint8_t *frame = hf_capture + frames * KIM_HF_FRAME_BYTES;
                    if (esp_codec_dev_read(mic, frame, KIM_HF_FRAME_BYTES) != ESP_CODEC_DEV_OK) {
                        mic_ok = false;
                        break;
                    }
                    ++frames;
                    if (kim_handsfree_frame(&hf, frame)) { complete = true; break; }
                }
                esp_codec_dev_close(mic);
                if (mic_ok && complete) {
                    size_t utterance = kim_handsfree_length(&hf);
                    kim_trial_capture(&kim_ota_trial, capture_nonzero(hf_capture, utterance));
                    kim_voice_state = KIM_THINKING;
                    send_response(utterance);
                } else if (mic_ok) {
                    /* No endpoint: keep or restore IDLE quietly. */
                } else {
                    kim_reply_failed = true;
                    ESP_LOGW(TAG, "handsfree mic read failed");
                }
            } else {
                kim_reply_failed = true;
                ESP_LOGW(TAG, "handsfree mic open failed code=%d muted=%d", open_result, muted);
            }
            memset(hf_capture, 0, HANDSFREE_CAPTURE); /* raw audio never persists */
            consumed = atomic_load(&kim_touch_ptt.epoch);
            kim_runtime_voice_end();
        }
#endif
        if (turn != consumed) {
            consumed = turn; /* every press is single-use, even when ineligible */
            if (!kim_touch_recording(&kim_touch_ptt, turn) || muted || !kim_is_online ||
                !kim_backend_healthy || !kim_runtime_voice_begin())
                goto wait_input;
            active_turn = turn;
            kim_voice_state = KIM_LISTENING;
            kim_reply_failed = false;
            esp_codec_dev_sample_info_t fs = {.sample_rate = RATE, .channel = 1, .bits_per_sample = 16};
            int open_result = esp_codec_dev_open(mic, &fs);
            if (open_result == ESP_CODEC_DEV_OK) {
                size_t size = 0;
                bool audio_ok = true;
                kim_vad_t vad;
                kim_vad_reset(&vad);
                bool speech_detected = false;
                while (kim_touch_recording(&kim_touch_ptt, turn) && gpio_get_level(BSP_BUTTON_MUTE_IO) != 0 &&
                       kim_is_online && size + 320 <= MAX_CAPTURE) {
                    if (esp_codec_dev_read(mic, capture + size, 320) != ESP_CODEC_DEV_OK) {
                        audio_ok = false;
                        break;
                    }
                    /* 10 ms / 160 samples. VAD observes PTT audio but does not
                     * prematurely cut off speech or bypass the mute switch. */
                    int16_t samples[160];
                    for (size_t i = 0; i < 160; ++i)
                        samples[i] = (int16_t)le16(capture + size + i * 2);
                    if (kim_vad_feed(&vad, samples, 160)) speech_detected = true;
                    size += 320;
                }
                esp_codec_dev_close(mic);
                ESP_LOGD(TAG, "PTT VAD speech=%d bytes=%u", speech_detected, (unsigned)size);
                /* Reaching the eight-second cap does not turn a still-held
                 * gesture into a release. Stop reading and await release. */
                while (size == MAX_CAPTURE && kim_touch_recording(&kim_touch_ptt, turn) &&
                       gpio_get_level(BSP_BUTTON_MUTE_IO) != 0 && kim_is_online)
                    vTaskDelay(pdMS_TO_TICKS(20));
                if (audio_ok && kim_touch_submit(&kim_touch_ptt, turn, size,
                      gpio_get_level(BSP_BUTTON_MUTE_IO) == 0, kim_is_online)) {
                    kim_trial_capture(&kim_ota_trial, capture_nonzero(capture, size));
                    kim_voice_state = KIM_THINKING;
                    send_response(size);
                }
                memset(capture, 0, size); /* Raw audio never persisted. */
            } else {
                kim_reply_failed = true;
                ESP_LOGW(TAG, "mic open failed code=%d muted=%d", open_result, muted);
            }
            /* A press interrupting a response is consumed, not recaptured
             * while still held. Only a subsequent fresh press starts a turn. */
            consumed = atomic_load(&kim_touch_ptt.epoch);
            kim_runtime_voice_end();
        }
wait_input:
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

#endif /* KIM_VOICE_HOST_TEST */
