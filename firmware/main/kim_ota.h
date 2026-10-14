#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Manual, asynchronous OTA. A pending trial is confirmed only after a real
 * captured PTT/reply, menu cycle and live display/audio/network health. */
esp_err_t kim_ota_init(void);
esp_err_t kim_ota_check(void);
esp_err_t kim_ota_install(void);
bool kim_ota_busy(void);
bool kim_ota_available(void);
void kim_ota_status(char *out, size_t len);
void kim_ota_trial_poll(bool display, bool audio, bool wifi, bool backend);
