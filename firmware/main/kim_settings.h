#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Call after nvs_flash_init(), before starting Wi-Fi or the UI. */
esp_err_t kim_settings_init(void);
int kim_settings_volume(void);
int kim_settings_brightness(void);
int kim_settings_sleep_s(void);
esp_err_t kim_settings_set_volume(int value);
esp_err_t kim_settings_set_brightness(int value);
esp_err_t kim_settings_set_sleep_s(int value);

#define KIM_SETTINGS_MAX_NETWORKS 8
typedef struct {
    char ssid[33];
    char password[64];
} kim_settings_network_t;
/* All network operations and NVS commits are mutex-serialized with settings
 * setters and readers. Call init once at bootstrap before workers start. */
int kim_settings_network_count(void);
bool kim_settings_network_get(int index, kim_settings_network_t *out);
esp_err_t kim_settings_network_save(const char *ssid, const char *password);
esp_err_t kim_settings_network_forget(const char *ssid);
