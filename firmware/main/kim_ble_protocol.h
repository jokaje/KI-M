#pragma once
#include <stddef.h>
#include <stdint.h>
#define KIM_BLE_MAX_REQUEST 512
/* Restrict SSIDs to printable ASCII; avoid ambiguous Unicode/escaping on Wi-Fi APIs. */
typedef struct {
    uint32_t id;
    uint32_t session; /* internal window generation; never sent on wire */
    char op[16];
    char ssid[33];
    char password[64];
    int volume, brightness, sleep_s;
    unsigned fields;
} kim_ble_request_t;
typedef enum { KIM_BLE_VALID=0, KIM_BLE_INVALID=1 } kim_ble_parse_result_t;
kim_ble_parse_result_t kim_ble_parse(const void *data, size_t len, kim_ble_request_t *out);
void kim_ble_wipe(kim_ble_request_t *out);
