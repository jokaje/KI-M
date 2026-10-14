#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { KIM_BLE_CLOSED, KIM_BLE_PENDING, KIM_BLE_ACTIVE,
               KIM_BLE_STOPPING, KIM_BLE_FAILED } kim_ble_state_t;
/* No radio/controller allocations at boot. */
esp_err_t kim_ble_init(void);
/* Enqueues intent only; safe in the LVGL writer callback. */
esp_err_t kim_ble_open(void);
void kim_ble_close(void);
void kim_ble_tick(void);
kim_ble_state_t kim_ble_state(void);
bool kim_ble_active(void);
/* LVGL writer only: copy ephemeral six-digit display code; false hides it. */
bool kim_ble_passkey(char out[7]);
