#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct { char ssid[33]; int rssi; bool saved; bool secure; } kim_wifi_ap_t;
/* Requires kim_settings_init() and initialized NVS. Queued actions never wait for radio. */
esp_err_t kim_wifi_start(const char *initial_ssid, const char *initial_password);
bool kim_wifi_connected(void);
esp_err_t kim_wifi_scan(void);
/* Generation increases only when fresh radio results have been consumed.
 * busy remains true while a request is queued or radio scan is underway. */
unsigned kim_wifi_scan_generation(void);
bool kim_wifi_scan_busy(void);
int kim_wifi_scan_results(kim_wifi_ap_t *out, int capacity);
esp_err_t kim_wifi_save(const char *ssid, const char *password);
esp_err_t kim_wifi_forget(const char *ssid);
/* IDs are monotonically increasing; result returns false until persistence completes. */
esp_err_t kim_wifi_save_tracked(const char *ssid, const char *password, unsigned *id);
esp_err_t kim_wifi_forget_tracked(const char *ssid, unsigned *id);
bool kim_wifi_operation_result(unsigned id, esp_err_t *result);
void kim_wifi_current_ssid(char out[33]);
void kim_wifi_status(char *out, size_t len);
