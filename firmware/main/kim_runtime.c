#include "kim_runtime.h"
#include <stdatomic.h>
/* Atomic compare-exchange is nonblocking on both cores; ownership spans the
 * entire turn, including network and speaker, not just microphone capture. */
static atomic_bool maintenance;
static atomic_bool voice_busy;
/* 0 stable/admissible, 1 probe owns crypto, 2 BLE transition owns pools.
 * Never hold an atomic spinlock across the network or a NimBLE call. */
static atomic_int resource_owner;
bool kim_tls_probe_begin(void) {
    int idle=0;
    return atomic_compare_exchange_strong(&resource_owner,&idle,1);
}
void kim_tls_probe_end(void) { atomic_store(&resource_owner,0); }
bool kim_ble_resource_begin(void) {
    int idle=0;
    return atomic_compare_exchange_strong(&resource_owner,&idle,2);
}
void kim_ble_resource_end(void) { atomic_store(&resource_owner,0); }
bool kim_maintenance_acquire(void) {
    bool free = false;
    return atomic_compare_exchange_strong(&maintenance, &free, true);
}
void kim_maintenance_release(void) { atomic_store(&maintenance, false); }
bool kim_runtime_voice_begin(void) {
    if (!kim_maintenance_acquire()) return false;
    atomic_store(&voice_busy, true);
    return true;
}
void kim_runtime_voice_end(void) {
    atomic_store(&voice_busy, false);
    kim_maintenance_release();
}
bool kim_runtime_voice_busy(void) { return atomic_load(&voice_busy); }
