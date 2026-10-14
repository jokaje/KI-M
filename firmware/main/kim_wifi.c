#include "kim_wifi.h"
#include "kim_settings.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MAX_APS 24
#define CONNECT_TIMEOUT_MS 12000
#define RETRY_BACKOFF_MS 1500
#define CYCLE_BACKOFF_MS 10000
/* The event callback only enqueues; all radio and NVS operations run on worker. */
enum { ACTION_SCAN = 1, ACTION_SAVE, ACTION_FORGET, EVENT_SCANNED, EVENT_DISCONNECTED, EVENT_GOT_IP };
typedef struct { int kind; unsigned id; char ssid[33]; char password[64]; } action_t;
static QueueHandle_t actions;
static atomic_bool started, connected;
static atomic_int state; /* 0 offline, 1 scanning, 2 connecting, 3 online */
static atomic_uint scan_generation;
static atomic_bool scan_busy;
static atomic_uint next_operation;
typedef struct { unsigned id; esp_err_t result; bool done; } operation_t;
static operation_t operations[16];
static portMUX_TYPE data_lock = portMUX_INITIALIZER_UNLOCKED;
static kim_wifi_ap_t visible[MAX_APS];
static int visible_count;
static char current[33];
static void lock(void) { portENTER_CRITICAL(&data_lock); }
static void unlock(void) { portEXIT_CRITICAL(&data_lock); }
static void operation_done(unsigned id, esp_err_t result) {
    if (!id) return;
    lock();
    operation_t *op = &operations[id % 16];
    if (op->id == id) { op->result = result; op->done = true; }
    unlock();
}
bool kim_wifi_operation_result(unsigned id, esp_err_t *result) {
    if (!id || !result) return false;
    lock();
    operation_t op = operations[id % 16];
    unlock();
    if (op.id != id || !op.done) return false;
    *result = op.result;
    return true;
}
static size_t bounded_len(const char *s, size_t max) {
    if (!s) return max;
    size_t i = 0;
    while (i < max && s[i]) ++i;
    return i;
}
static bool ssid_ok(const char *s) { size_t n = bounded_len(s, 33); return n > 0 && n <= 32; }
static bool password_ok(const char *s) { size_t n = bounded_len(s, 64); return n >= 8 && n <= 63; }
static bool supported(int mode) {
    return mode == WIFI_AUTH_WPA2_PSK || mode == WIFI_AUTH_WPA3_PSK ||
           mode == WIFI_AUTH_WPA2_WPA3_PSK;
}
static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *payload) {
    (void)ctx; (void)payload;
    action_t a = {0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) a.kind = EVENT_SCANNED;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&connected, false);
        atomic_store(&state, 0);
        a.kind = EVENT_DISCONNECTED;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&connected, true);
        atomic_store(&state, 3);
        a.kind = EVENT_GOT_IP;
    }
    if (a.kind && actions) xQueueSend(actions, &a, 0);
}
static int preference(const char *ssid) {
    kim_settings_network_t net;
    int count = kim_settings_network_count();
    for (int i = 0; i < count; ++i)
        if (kim_settings_network_get(i, &net) && !strcmp(net.ssid, ssid)) {
            memset(&net, 0, sizeof(net));
            return i;
        }
    memset(&net, 0, sizeof(net));
    return -1;
}
static void scan_done(void) {
    wifi_ap_record_t records[MAX_APS] = {0};
    uint16_t n = MAX_APS;
    if (esp_wifi_scan_get_ap_records(&n, records) != ESP_OK) {
        lock(); visible_count = 0; unlock();
        atomic_store(&scan_busy, false);
        return;
    }
    kim_wifi_ap_t result[MAX_APS] = {0};
    int used = 0;
    for (uint16_t i = 0; i < n; ++i) {
        size_t len = bounded_len((const char *)records[i].ssid, sizeof(records[i].ssid));
        if (len == 0 || len > 32) continue;
        int existing = -1;
        for (int j = 0; j < used; ++j)
            if (!strcmp(result[j].ssid, (const char *)records[i].ssid)) { existing = j; break; }
        bool secure = supported(records[i].authmode);
        if (existing >= 0 && (result[existing].secure > secure ||
            (result[existing].secure == secure && result[existing].rssi >= records[i].rssi))) continue;
        kim_wifi_ap_t *ap = existing >= 0 ? &result[existing] : &result[used++];
        memset(ap, 0, sizeof(*ap));
        memcpy(ap->ssid, records[i].ssid, len);
        ap->rssi = records[i].rssi;
        ap->secure = secure;
        ap->saved = preference(ap->ssid) >= 0;
    }
    lock();
    memcpy(visible, result, (size_t)used * sizeof(result[0]));
    visible_count = used;
    unlock();
    atomic_fetch_add(&scan_generation, 1);
    atomic_store(&scan_busy, false);
}
static bool connect_best(unsigned char failures[KIM_SETTINGS_MAX_NETWORKS]) {
    kim_wifi_ap_t aps[MAX_APS];
    int n;
    lock(); n = visible_count; memcpy(aps, visible, (size_t)n * sizeof(aps[0])); unlock();
    int best = -1, rank = KIM_SETTINGS_MAX_NETWORKS, rssi = -128;
    for (int i = 0; i < n; ++i) {
        int index = preference(aps[i].ssid);
        if (index >= 0 && aps[i].secure && failures[index] < 2 &&
            (index < rank || (index == rank && aps[i].rssi > rssi))) {
            best = i; rank = index; rssi = aps[i].rssi;
        }
    }
    if (best < 0) return false;
    kim_settings_network_t net;
    if (!kim_settings_network_get(rank, &net)) return false;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, net.ssid, strlen(net.ssid));
    memcpy(config.sta.password, net.password, strlen(net.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    /* WPA3-only APs require PMF; mixed WPA2/WPA3 is negotiated by IDF. */
    config.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) err = esp_wifi_connect();
    memset(&net, 0, sizeof(net));
    memset(&config, 0, sizeof(config));
    ++failures[rank];
    if (err != ESP_OK) return false;
    lock(); memcpy(current, aps[best].ssid, sizeof(current)); unlock();
    atomic_store(&state, 2);
    return true;
}
static void worker(void *arg) {
    (void)arg;
    unsigned char failures[KIM_SETTINGS_MAX_NETWORKS] = {0};
    bool scanning = false, connecting = false, manual_pending = false;
    TickType_t deadline = 0, next_scan = 0;
    for (;;) {
        action_t a = {0};
        bool received = xQueueReceive(actions, &a, pdMS_TO_TICKS(250)) == pdTRUE;
        TickType_t now = xTaskGetTickCount();
        if (received) {
            if (a.kind == ACTION_SAVE) {
                esp_err_t saved = kim_settings_network_save(a.ssid, a.password);
                operation_done(a.id, saved);
                if (saved == ESP_OK) {
                    memset(failures, 0, sizeof(failures));
                    if (atomic_load(&connected)) esp_wifi_disconnect();
                    atomic_store(&connected, false); atomic_store(&state, 0); connecting = false;
                    next_scan = 0;
                }
            } else if (a.kind == ACTION_FORGET) {
                esp_err_t forgotten = kim_settings_network_forget(a.ssid);
                operation_done(a.id, forgotten);
                if (forgotten == ESP_OK) {
                    memset(failures, 0, sizeof(failures));
                    char active[33]; kim_wifi_current_ssid(active);
                    if (!strcmp(active, a.ssid)) {
                        esp_wifi_disconnect(); atomic_store(&connected, false);
                        atomic_store(&state, 0); connecting = false;
                        lock(); current[0] = 0; unlock();
                    }
                    next_scan = 0;
                }
            } else if (a.kind == ACTION_SCAN) {
                manual_pending = true;
                next_scan = 0;
            }
            else if (a.kind == EVENT_SCANNED) {
                scanning = false;
                scan_done();
                if (!atomic_load(&connected) && !connecting) {
                    connecting = connect_best(failures);
                    deadline = now + pdMS_TO_TICKS(CONNECT_TIMEOUT_MS);
                    if (!connecting) {
                        bool exhausted = true;
                        for (int i = 0; i < kim_settings_network_count(); ++i)
                            if (failures[i] < 2) exhausted = false;
                        if (exhausted) { memset(failures, 0, sizeof(failures)); next_scan = now + pdMS_TO_TICKS(CYCLE_BACKOFF_MS); }
                        else next_scan = now + pdMS_TO_TICKS(RETRY_BACKOFF_MS);
                    }
                }
            } else if (a.kind == EVENT_DISCONNECTED) {
                connecting = false;
                next_scan = now + pdMS_TO_TICKS(RETRY_BACKOFF_MS);
            } else if (a.kind == EVENT_GOT_IP) {
                connecting = false;
                memset(failures, 0, sizeof(failures));
            }
            memset(&a, 0, sizeof(a));
        }
        if (connecting && !atomic_load(&connected) && (int32_t)(now - deadline) >= 0) {
            connecting = false;
            esp_wifi_disconnect();
            next_scan = now + pdMS_TO_TICKS(RETRY_BACKOFF_MS);
        }
        if (scanning && (int32_t)(now - deadline) >= 0) {
            scanning = false;
            atomic_store(&scan_busy, false);
            next_scan = now + pdMS_TO_TICKS(RETRY_BACKOFF_MS);
        }
        if (!scanning && !connecting && (int32_t)(now - next_scan) >= 0 &&
            (!atomic_load(&connected) || manual_pending)) {
            wifi_scan_config_t cfg = {.show_hidden = false};
            atomic_store(&scan_busy, true);
            if (esp_wifi_scan_start(&cfg, false) == ESP_OK) {
                manual_pending = false;
                scanning = true; deadline = now + pdMS_TO_TICKS(CONNECT_TIMEOUT_MS);
                if (!atomic_load(&connected)) atomic_store(&state, 1);
            } else { manual_pending = false; atomic_store(&scan_busy, false); next_scan = now + pdMS_TO_TICKS(RETRY_BACKOFF_MS); }
        }
    }
}
esp_err_t kim_wifi_start(const char *initial_ssid, const char *initial_password) {
    if (atomic_load(&started)) return ESP_ERR_INVALID_STATE;
    if (kim_settings_network_count() == 0 && initial_ssid && initial_ssid[0]) {
        if (!ssid_ok(initial_ssid) || !password_ok(initial_password)) return ESP_ERR_INVALID_ARG;
        esp_err_t err = kim_settings_network_save(initial_ssid, initial_password);
        if (err != ESP_OK) return err;
    }
    actions = xQueueCreate(16, sizeof(action_t));
    if (!actions) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK ||
        (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) return err;
    if (xTaskCreate(worker, "kim-wifi", 8192, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    atomic_store(&started, true);
    return ESP_OK;
}
bool kim_wifi_connected(void) { return atomic_load(&connected); }
esp_err_t kim_wifi_scan(void) {
    if (!atomic_load(&started)) return ESP_ERR_INVALID_STATE;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&scan_busy, &idle, true)) return ESP_ERR_INVALID_STATE;
    action_t a = {.kind = ACTION_SCAN};
    if (xQueueSend(actions, &a, 0) == pdTRUE) return ESP_OK;
    atomic_store(&scan_busy, false);
    return ESP_ERR_NO_MEM;
}
unsigned kim_wifi_scan_generation(void) { return atomic_load(&scan_generation); }
bool kim_wifi_scan_busy(void) { return atomic_load(&scan_busy); }
int kim_wifi_scan_results(kim_wifi_ap_t *out, int capacity) {
    if (capacity < 0 || (capacity && !out)) return 0;
    lock();
    int n = capacity < visible_count ? capacity : visible_count;
    if (n) memcpy(out, visible, (size_t)n * sizeof(visible[0]));
    unlock();
    return n;
}
esp_err_t kim_wifi_save_tracked(const char *ssid, const char *password, unsigned *id) {
    if (!ssid_ok(ssid) || !password_ok(password)) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&started)) return ESP_ERR_INVALID_STATE;
    action_t a = {.kind = ACTION_SAVE};
    if (id) {
        a.id = atomic_fetch_add(&next_operation, 1) + 1;
        lock(); operations[a.id % 16] = (operation_t){.id = a.id}; unlock();
    }
    memcpy(a.ssid, ssid, strlen(ssid));
    memcpy(a.password, password, strlen(password));
    esp_err_t err = xQueueSend(actions, &a, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && id) *id = a.id;
    memset(&a, 0, sizeof(a));
    return err;
}
esp_err_t kim_wifi_save(const char *ssid, const char *password) {
    return kim_wifi_save_tracked(ssid, password, NULL);
}
esp_err_t kim_wifi_forget_tracked(const char *ssid, unsigned *id) {
    if (!ssid_ok(ssid)) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&started)) return ESP_ERR_INVALID_STATE;
    action_t a = {.kind = ACTION_FORGET};
    if (id) {
        a.id = atomic_fetch_add(&next_operation, 1) + 1;
        lock(); operations[a.id % 16] = (operation_t){.id = a.id}; unlock();
    }
    memcpy(a.ssid, ssid, strlen(ssid));
    esp_err_t err = xQueueSend(actions, &a, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && id) *id = a.id;
    return err;
}
esp_err_t kim_wifi_forget(const char *ssid) {
    return kim_wifi_forget_tracked(ssid, NULL);
}
void kim_wifi_current_ssid(char out[33]) {
    if (!out) return;
    lock(); memcpy(out, current, sizeof(current)); unlock();
}
void kim_wifi_status(char *out, size_t len) {
    if (!out || !len) return;
    static const char *const labels[] = {"Offline", "Scanning", "Connecting", "Online"};
    int s = atomic_load(&state);
    snprintf(out, len, "%s", labels[s >= 0 && s <= 3 ? s : 0]);
}
