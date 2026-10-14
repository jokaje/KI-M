/* Private, manual A/B OTA. The parent provides the shared voice/maintenance gate. */
#include "kim_ota.h"
#include "kim_ota_policy.h"
#include "kim_trial.h"
#include "kim_runtime.h"
#include "voice.h"
#include "sdkconfig.h"
#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "KI-M OTA requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y"
#endif
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"
#if __has_include("config_local.h")
#include "config_local.h"
#else
#include "config_local.example.h"
#endif
#ifndef KIM_OTA_MANIFEST_URL
#define KIM_OTA_MANIFEST_URL ""
#endif
#ifndef KIM_OTA_BASE_URL
#define KIM_OTA_BASE_URL ""
#endif
#ifndef KIM_OTA_PUBLIC_KEY_PEM
#define KIM_OTA_PUBLIC_KEY_PEM ""
#endif

static const char *TAG = "kim-ota";
static atomic_bool working;
static atomic_bool ready;
static atomic_bool initialized;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static char state[64] = "Not initialized";
static kim_ota_manifest_t candidate;
kim_trial_t kim_ota_trial;
#define OTA_MANIFEST_MAX 512
#define OTA_HTTP_MS 8000
#define OTA_TOTAL_US (180LL * 1000000)

static void status_set(const char *text) {
    portENTER_CRITICAL(&lock);
    snprintf(state, sizeof(state), "%s", text);
    portEXIT_CRITICAL(&lock);
}
void kim_ota_status(char *out, size_t len) {
    if (!out || !len) return;
    portENTER_CRITICAL(&lock);
    snprintf(out, len, "%s", state);
    portEXIT_CRITICAL(&lock);
}
bool kim_ota_busy(void) { return atomic_load(&working); }
bool kim_ota_available(void) { return atomic_load(&ready); }

static bool pending_verify(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state_value;
    return !running || (esp_ota_get_state_partition(running, &state_value) == ESP_OK &&
                        state_value == ESP_OTA_IMG_PENDING_VERIFY);
}

static esp_http_client_handle_t request(const char *url) {
    esp_http_client_config_t cfg = {
        .url = url, .cert_pem = KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL,
        .crt_bundle_attach = KIM_SERVER_CA_PEM[0] ? NULL : esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_MS,
        .disable_auto_redirect = true, .max_authorization_retries = -1,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char auth[256];
    if (snprintf(auth,sizeof(auth),"Bearer %s",KIM_DEVICE_TOKEN) >= sizeof(auth) ||
        esp_http_client_set_header(c,"Authorization",auth) != ESP_OK ||
        esp_http_client_set_header(c,"Accept-Encoding","identity") != ESP_OK ||
        esp_http_client_open(c,0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    int64_t length = esp_http_client_fetch_headers(c);
    if (length < 0 || esp_http_client_get_status_code(c) != 200 ||
        esp_http_client_is_chunked_response(c)) {
        esp_http_client_cleanup(c); return NULL;
    }
    return c;
}
static void close_request(esp_http_client_handle_t c) {
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
}
static bool signature_valid(const char *bytes, const kim_ota_manifest_t *m) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    unsigned char hash[32];
    int ok = mbedtls_pk_parse_public_key(&pk,(const unsigned char *)KIM_OTA_PUBLIC_KEY_PEM,
                                        strlen(KIM_OTA_PUBLIC_KEY_PEM)+1);
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    if (!ok) ok = mbedtls_pk_get_psa_attributes(&pk, PSA_KEY_USAGE_VERIFY_HASH, &attrs);
    if (!ok && (psa_get_key_type(&attrs) != PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1) ||
                psa_get_key_bits(&attrs) != 256)) ok = -1;
    psa_reset_key_attributes(&attrs);
    size_t hash_len = 0;
    if (!ok) ok = psa_hash_compute(PSA_ALG_SHA_256,(const unsigned char *)bytes,
                                    m->signed_len,hash,sizeof(hash),&hash_len) == PSA_SUCCESS ? 0 : -1;
    if (!ok && hash_len != sizeof(hash)) ok = -1;
    if (!ok) ok = mbedtls_pk_verify(&pk,MBEDTLS_MD_SHA256,hash,sizeof(hash),
                                    m->signature,m->signature_len);
    mbedtls_pk_free(&pk);
    return ok == 0;
}
static bool timed_out(int64_t start) { return esp_timer_get_time() - start > OTA_TOTAL_US; }
static bool check_manifest(kim_ota_manifest_t *out) {
    esp_http_client_handle_t c = request(KIM_OTA_MANIFEST_URL);
    if (!c) return false;
    char buf[OTA_MANIFEST_MAX+1]; size_t used = 0;
    int64_t declared = esp_http_client_get_content_length(c);
    bool ok = declared >= 120 && declared <= OTA_MANIFEST_MAX;
    int64_t start = esp_timer_get_time();
    while (ok && used < (size_t)declared && !timed_out(start)) {
        int got = esp_http_client_read(c,buf+used,(int)((size_t)declared-used));
        if (got <= 0) { ok = false; break; }
        used += (size_t)got;
    }
    ok = ok && used == (size_t)declared && !timed_out(start) &&
         esp_http_client_is_complete_data_received(c) &&
         kim_ota_parse(buf,used,out) && signature_valid(buf,out) &&
         kim_ota_version_compare(out->version,esp_app_get_description()->version) == 1;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    ok = ok && partition && out->size <= partition->size;
    close_request(c);
    return ok;
}
static bool image_descriptor(const uint8_t *bytes, size_t count, const kim_ota_manifest_t *m) {
    size_t offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (count < offset + sizeof(esp_app_desc_t) || bytes[0] != ESP_IMAGE_HEADER_MAGIC) return false;
    esp_image_header_t header;
    memcpy(&header,bytes,sizeof(header));
    if (header.chip_id != ESP_CHIP_ID_ESP32S3) return false;
    esp_app_desc_t d;
    memcpy(&d,bytes+offset,sizeof(d));
    return d.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           memchr(d.version,0,sizeof(d.version)) &&
           memchr(d.project_name,0,sizeof(d.project_name)) &&
           strcmp(d.version,m->version) == 0 && strcmp(d.project_name,"ki-m-box3") == 0;
}
static bool install_image(const kim_ota_manifest_t *m) {
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || m->size > target->size) return false;
    char url[256];
    if (snprintf(url,sizeof(url),"%s%s",KIM_OTA_BASE_URL,m->image) >= sizeof(url)) return false;
    esp_http_client_handle_t c = request(url);
    if (!c) return false;
    bool ok = esp_http_client_get_content_length(c) == (int64_t)m->size;
    esp_ota_handle_t handle = 0;
    bool begun = false;
    psa_hash_operation_t digest = PSA_HASH_OPERATION_INIT;
    if (ok) ok = psa_hash_setup(&digest,PSA_ALG_SHA_256) == PSA_SUCCESS;
    size_t received = 0;
    uint8_t buf[2048];
    size_t buffered = 0;
    const size_t descriptor_end = sizeof(esp_image_header_t) +
                                  sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    int64_t start = esp_timer_get_time();
    while (ok && received < m->size && !timed_out(start)) {
        size_t remaining = m->size-received-buffered;
        size_t want = remaining < sizeof(buf)-buffered ? remaining : sizeof(buf)-buffered;
        int got = esp_http_client_read(c,(char *)buf+buffered,(int)want);
        if (got <= 0) { ok = false; break; }
        buffered += (size_t)got;
        if (!begun) {
            if (buffered < descriptor_end) continue;
            if (!image_descriptor(buf,buffered,m) || esp_ota_begin(target,m->size,&handle) != ESP_OK) {
                ok = false; break;
            }
            begun = true;
        }
        if (psa_hash_update(&digest,buf,buffered) != PSA_SUCCESS ||
            esp_ota_write(handle,buf,buffered) != ESP_OK) { ok = false; break; }
        received += buffered;
        buffered = 0;
    }
    unsigned char actual[32];
    size_t actual_len = 0;
    if (ok) ok = received == m->size && !timed_out(start) &&
                 esp_http_client_is_complete_data_received(c) &&
                 psa_hash_finish(&digest,actual,sizeof(actual),&actual_len) == PSA_SUCCESS &&
                 actual_len == sizeof(actual) && memcmp(actual,m->sha256,sizeof(actual)) == 0;
    if (!ok) psa_hash_abort(&digest);
    close_request(c);
    if (begun) {
        if (ok) { ok = esp_ota_end(handle) == ESP_OK; begun = false; }
        if (begun) esp_ota_abort(handle);
    }
    if (ok) ok = esp_ota_set_boot_partition(target) == ESP_OK;
    return ok;
}
static void worker(void *arg) {
    bool install = (uintptr_t)arg != 0;
    bool gated = kim_maintenance_acquire();
    if (!gated || kim_runtime_voice_busy()) {
        if (gated) kim_maintenance_release();
        status_set("Voice busy; retry later");
        atomic_store(&working,false);
        vTaskDelete(NULL); return;
    }
    bool ok = false;
    if (!atomic_load(&kim_backend_healthy)) {
        status_set("Network/time/backend unavailable");
    } else if (pending_verify()) {
        status_set("Pending health confirmation");
    } else if (install && !atomic_load(&ready)) {
        status_set("Check for update first");
    } else if (install) {
        kim_ota_manifest_t fresh;
        status_set("Verifying update");
        ok = check_manifest(&fresh);
        if (ok) {
            portENTER_CRITICAL(&lock);
            ok = memcmp(&fresh,&candidate,sizeof(fresh)) == 0;
            portEXIT_CRITICAL(&lock);
        }
        if (ok) { status_set("Installing update"); ok = install_image(&fresh); }
        if (!ok) { atomic_store(&ready,false); status_set("Update failed; known-good retained"); }
        else { status_set("Rebooting to trial image"); esp_restart(); }
    } else {
        kim_ota_manifest_t found;
        status_set("Checking update");
        ok = check_manifest(&found);
        if (ok) {
            portENTER_CRITICAL(&lock);
            candidate = found;
            portEXIT_CRITICAL(&lock);
            atomic_store(&ready,true);
            status_set("Update available");
        } else { atomic_store(&ready,false); status_set("No valid newer update"); }
    }
    kim_maintenance_release();
    atomic_store(&working,false);
    vTaskDelete(NULL);
}
esp_err_t kim_ota_init(void) {
    /* Arm rollback even on an unprovisioned image: trial must not remain pending forever. */
    kim_trial_start(&kim_ota_trial, pending_verify(), (uint32_t)(esp_timer_get_time() / 1000));
    if (!KIM_OTA_MANIFEST_URL[0] || !KIM_OTA_BASE_URL[0] || !KIM_OTA_PUBLIC_KEY_PEM[0] ||
        strlen(KIM_DEVICE_TOKEN) < 32 ||
        !kim_ota_same_origin(KIM_OTA_MANIFEST_URL,KIM_OTA_BASE_URL) ||
        KIM_OTA_BASE_URL[strlen(KIM_OTA_BASE_URL)-1] != '/' ||
        strchr(KIM_OTA_MANIFEST_URL,'?') || strchr(KIM_OTA_BASE_URL,'?') ||
        strchr(KIM_OTA_MANIFEST_URL,'#') || strchr(KIM_OTA_BASE_URL,'#') ||
        !esp_ota_get_next_update_partition(NULL)) {
        status_set("OTA provisioning/partition invalid"); return ESP_ERR_INVALID_STATE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        status_set("OTA crypto unavailable"); return ESP_ERR_INVALID_STATE;
    }
    if (kim_ota_trial.pending) status_set("Trial: PTT reply + menu required (180s)");
    else status_set("Ready to check");
    atomic_store(&initialized,true);
    return ESP_OK;
}
static esp_err_t start(bool install) {
    if (!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&working,&expected,true)) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(worker,"kim-ota",8192,(void *)(uintptr_t)install,3,NULL) != pdPASS) {
        atomic_store(&working,false); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t kim_ota_check(void) { return start(false); }
esp_err_t kim_ota_install(void) { return start(true); }
static void kim_ota_confirm_healthy(void) {
    /* Parent must prove all subsystems healthy before invoking this function.
     * Never called automatically by init, check, or install. */
    if (!kim_ota_trial.pending || !pending_verify()) return;
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK)
        ESP_LOGE(TAG,"Health confirmation failed; rollback remains armed");
    else { kim_ota_trial.pending = false; status_set("Trial confirmed"); }
}
void kim_ota_trial_poll(bool display, bool audio, bool wifi, bool backend) {
    if (!kim_ota_trial.pending) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (kim_trial_expired(&kim_ota_trial, now)) {
        status_set("Trial timed out; rollback and reboot");
        if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK)
            status_set("Rollback failed; trial unconfirmed, reboot required");
        return;
    }
    if (atomic_load(&initialized) &&
        kim_trial_ready(&kim_ota_trial, display, audio, wifi, backend))
        kim_ota_confirm_healthy();
}
