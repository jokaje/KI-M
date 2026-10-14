#pragma once
#include <stdbool.h>
/* One nonblocking maintenance lock spans capture, upload and playback. */
bool kim_maintenance_acquire(void);
void kim_maintenance_release(void);
bool kim_runtime_voice_begin(void);
void kim_runtime_voice_end(void);
bool kim_runtime_voice_busy(void);
/* TLS probes run outside maintenance. A transition admits no new probes;
 * the BLE manager waits for an already admitted probe before touching pools. */
bool kim_tls_probe_begin(void);
void kim_tls_probe_end(void);
bool kim_ble_resource_begin(void);
void kim_ble_resource_end(void);
