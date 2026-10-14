#include "kim_settings.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SETTINGS_VERSION 1u
#define NETWORK_VERSION 1u
#define NAMESPACE "kim_device"

typedef struct { uint32_t version; int32_t volume, brightness, sleep_s; } preferences_t;
typedef struct { uint32_t version, count; kim_settings_network_t entries[KIM_SETTINGS_MAX_NETWORKS]; } networks_t;
static preferences_t prefs = {SETTINGS_VERSION, 50, 70, 300};
static networks_t networks = {.version = NETWORK_VERSION};
static atomic_int volume = 50, brightness = 70, sleep_s = 300;
static bool ready;
/* Bootstrap creates this before workers; serializes RAM and NVS transactions. */
static SemaphoreHandle_t settings_mutex;
static void acquire(void) { xSemaphoreTake(settings_mutex, portMAX_DELAY); }
static void release(void) { xSemaphoreGive(settings_mutex); }

static size_t bounded_len(const char *s, size_t max) {
    if (!s) return max;
    size_t i = 0;
    while (i < max && s[i]) ++i;
    return i;
}
static bool valid_ssid(const char *ssid) {
    size_t n = bounded_len(ssid, 33);
    return n > 0 && n <= 32;
}
static bool valid_password(const char *password) {
    size_t n = bounded_len(password, 64);
    return n >= 8 && n <= 63;
}
static esp_err_t store(const char *key, const void *blob, size_t size) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, key, blob, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
esp_err_t kim_settings_init(void) {
    if (!settings_mutex) {
        settings_mutex = xSemaphoreCreateMutex();
        if (!settings_mutex) return ESP_ERR_NO_MEM;
    }
    acquire();
    ready = false;
    prefs = (preferences_t){SETTINGS_VERSION, 50, 70, 300};
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) { release(); return err; }
    preferences_t loaded = {0};
    size_t size = sizeof(loaded);
    err = nvs_get_blob(handle, "prefs", &loaded, &size);
    if (err == ESP_OK && size == sizeof(loaded) && loaded.version == SETTINGS_VERSION &&
        loaded.volume >= 0 && loaded.volume <= 80 &&
        loaded.brightness >= 10 && loaded.brightness <= 100 &&
        (loaded.sleep_s == 0 || (loaded.sleep_s >= 30 && loaded.sleep_s <= 1800))) prefs = loaded;
    else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH) { nvs_close(handle); release(); return err; }
    networks_t saved = {0};
    size = sizeof(saved);
    err = nvs_get_blob(handle, "networks", &saved, &size);
    nvs_close(handle);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH) { release(); return err; }
    networks = (networks_t){.version = NETWORK_VERSION};
    if (err == ESP_OK && size == sizeof(saved) && saved.version == NETWORK_VERSION && saved.count <= KIM_SETTINGS_MAX_NETWORKS) {
        bool valid = true;
        for (unsigned i = 0; i < saved.count; ++i) {
            if (!valid_ssid(saved.entries[i].ssid) || !valid_password(saved.entries[i].password)) valid = false;
            for (unsigned j = 0; j < i; ++j)
                if (!strcmp(saved.entries[i].ssid, saved.entries[j].ssid)) valid = false;
        }
        if (valid) networks = saved;
    }
    atomic_store(&volume, prefs.volume);
    atomic_store(&brightness, prefs.brightness);
    atomic_store(&sleep_s, prefs.sleep_s);
    ready = true;
    release();
    return ESP_OK;
}
int kim_settings_volume(void) { return atomic_load(&volume); }
int kim_settings_brightness(void) { return atomic_load(&brightness); }
int kim_settings_sleep_s(void) { return atomic_load(&sleep_s); }
static esp_err_t update(int field, int value) {
    if ((field == 0 && (value < 0 || value > 80)) ||
        (field == 1 && (value < 10 || value > 100)) ||
        (field == 2 && value != 0 && (value < 30 || value > 1800))) return ESP_ERR_INVALID_ARG;
    if (!settings_mutex) return ESP_ERR_INVALID_STATE;
    acquire();
    if (!ready) { release(); return ESP_ERR_INVALID_STATE; }
    preferences_t next = prefs;
    if (field == 0) next.volume = value;
    if (field == 1) next.brightness = value;
    if (field == 2) next.sleep_s = value;
    esp_err_t err = store("prefs", &next, sizeof(next));
    if (err == ESP_OK) {
        prefs = next;
        if (field == 0) atomic_store(&volume, value);
        if (field == 1) atomic_store(&brightness, value);
        if (field == 2) atomic_store(&sleep_s, value);
    }
    release();
    return err;
}
esp_err_t kim_settings_set_volume(int value) { return update(0, value); }
esp_err_t kim_settings_set_brightness(int value) { return update(1, value); }
esp_err_t kim_settings_set_sleep_s(int value) { return update(2, value); }
int kim_settings_network_count(void) {
    if (!settings_mutex) return 0;
    acquire(); int count = ready ? (int)networks.count : 0; release(); return count;
}
bool kim_settings_network_get(int index, kim_settings_network_t *out) {
    if (!settings_mutex || !out) return false;
    acquire();
    bool found = ready && index >= 0 && (unsigned)index < networks.count;
    if (found) *out = networks.entries[index];
    release(); return found;
}
esp_err_t kim_settings_network_save(const char *ssid, const char *password) {
    if (!valid_ssid(ssid) || !valid_password(password)) return ESP_ERR_INVALID_ARG;
    if (!settings_mutex) return ESP_ERR_INVALID_STATE;
    acquire();
    if (!ready) { release(); return ESP_ERR_INVALID_STATE; }
    networks_t next = networks;
    int old = -1;
    for (unsigned i = 0; i < next.count; ++i) if (!strcmp(next.entries[i].ssid, ssid)) old = (int)i;
    if (old < 0 && next.count == KIM_SETTINGS_MAX_NETWORKS) { memset(&next,0,sizeof next); release(); return ESP_ERR_NO_MEM; }
    if (old >= 0) memmove(&next.entries[old], &next.entries[old + 1], (next.count - (unsigned)old - 1) * sizeof(next.entries[0]));
    else ++next.count;
    memmove(&next.entries[1], &next.entries[0], (next.count - 1) * sizeof(next.entries[0]));
    memset(&next.entries[0], 0, sizeof(next.entries[0]));
    memcpy(next.entries[0].ssid, ssid, strlen(ssid));
    memcpy(next.entries[0].password, password, strlen(password));
    esp_err_t err = store("networks", &next, sizeof(next));
    if (err == ESP_OK) networks = next;
    memset(&next, 0, sizeof(next));
    release();
    return err;
}
esp_err_t kim_settings_network_forget(const char *ssid) {
    if (!valid_ssid(ssid)) return ESP_ERR_INVALID_ARG;
    if (!settings_mutex) return ESP_ERR_INVALID_STATE;
    acquire();
    if (!ready) { release(); return ESP_ERR_INVALID_STATE; }
    networks_t next = networks;
    unsigned i = 0;
    while (i < next.count && strcmp(next.entries[i].ssid, ssid)) ++i;
    if (i == next.count) { memset(&next,0,sizeof next); release(); return ESP_ERR_NVS_NOT_FOUND; }
    memmove(&next.entries[i], &next.entries[i + 1], (next.count - i - 1) * sizeof(next.entries[0]));
    memset(&next.entries[--next.count], 0, sizeof(next.entries[0]));
    esp_err_t err = store("networks", &next, sizeof(next));
    if (err == ESP_OK) networks = next;
    memset(&next, 0, sizeof(next));
    release();
    return err;
}
