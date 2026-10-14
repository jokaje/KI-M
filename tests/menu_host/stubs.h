#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
typedef int esp_err_t;
#define ESP_OK 0
typedef enum { KIM_BLE_CLOSED, KIM_BLE_PENDING, KIM_BLE_ACTIVE,
               KIM_BLE_STOPPING, KIM_BLE_FAILED } kim_ble_state_t;
static int ble_queued, ble_closes;
static kim_ble_state_t ble_state=KIM_BLE_CLOSED;
static inline esp_err_t kim_ble_open(void) { ble_queued++;ble_state=KIM_BLE_PENDING;return ESP_OK; }
static inline void kim_ble_close(void) { ble_closes++;ble_state=KIM_BLE_CLOSED; }
static inline kim_ble_state_t kim_ble_state(void) { return ble_state; }
static inline bool kim_ble_passkey(char out[7]) { (void)out;return false; }
#define ESP_ERR_TIMEOUT 1
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
typedef unsigned TickType_t;
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(int a, size_t b) { (void)a;(void)b;return (void *)1; }
static inline int xQueueReceive(QueueHandle_t q, void *p, unsigned t) { (void)q;(void)p;(void)t;return 0; }
static inline int xQueueSend(QueueHandle_t q, const void *p, unsigned t) { (void)q;(void)p;(void)t;return 1; }
static inline int xTaskCreate(void (*f)(void *), const char *n, int s, void *a, int pr, void *h) { (void)f;(void)n;(void)s;(void)a;(void)pr;(void)h;return pdPASS; }
static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline void vTaskDelay(unsigned t) { (void)t; }
#define GPIO_NUM_0 0
static inline int gpio_get_level(int g) { (void)g;return 1; }
static inline esp_err_t bsp_display_brightness_set(int v) { (void)v;return ESP_OK; }
#define ESP_LOGI(...) ((void)0)
static inline const char *esp_err_to_name(esp_err_t e) { (void)e;return "host"; }
typedef struct { char ssid[33]; bool saved, secure; } kim_wifi_ap_t;
typedef struct { char ssid[33]; } kim_settings_network_t;
typedef struct { int unused; } kim_trial_t;
static kim_trial_t kim_ota_trial;
static inline void kim_trial_menu(kim_trial_t *t, bool o) { (void)t;(void)o; }
static inline unsigned kim_wifi_scan_generation(void) { return 1; }
static inline bool kim_wifi_scan_busy(void) { return false; }
static inline esp_err_t kim_wifi_scan(void) { return ESP_OK; }
static inline int kim_wifi_scan_results(kim_wifi_ap_t *a, int n) { (void)a;(void)n;return 0; }
static inline int kim_settings_network_count(void) { return 0; }
static inline bool kim_settings_network_get(int i, kim_settings_network_t *n) { (void)i;(void)n;return false; }
static inline esp_err_t kim_wifi_save_tracked(const char *s, const char *p, unsigned *i) { (void)s;(void)p;(void)i;return ESP_OK; }
static inline esp_err_t kim_wifi_forget_tracked(const char *s, unsigned *i) { (void)s;(void)i;return ESP_OK; }
static inline bool kim_wifi_operation_result(unsigned i, esp_err_t *e) { (void)i;(void)e;return true; }
static inline int kim_settings_volume(void) { return 50; }
static inline int kim_settings_brightness(void) { return 65; }
static inline int kim_settings_sleep_s(void) { return 0; }
static inline esp_err_t kim_settings_set_volume(int v) { (void)v;return ESP_OK; }
static inline esp_err_t kim_settings_set_brightness(int v) { (void)v;return ESP_OK; }
static inline esp_err_t kim_settings_set_sleep_s(int v) { (void)v;return ESP_OK; }
static inline esp_err_t kim_ota_check(void) { return ESP_OK; }
static inline esp_err_t kim_ota_install(void) { return ESP_OK; }
static inline bool kim_ota_busy(void) { return false; }
static inline bool kim_ota_available(void) { return true; }
static inline void kim_ota_status(char *s, size_t n) { snprintf(s,n,"Bereit"); }
static inline void kim_wifi_status(char *s, size_t n) { snprintf(s,n,"WLAN bereit"); }
