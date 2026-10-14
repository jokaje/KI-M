/* Exercise the actual atomic TLS/BLE admission implementation. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include "kim_runtime.h"
static atomic_bool inflight, release_probe, acquired;
static void *probe(void *unused) {
    (void)unused;
    assert(kim_tls_probe_begin());
    atomic_store(&inflight,true);
    while(!atomic_load(&release_probe)) sched_yield();
    kim_tls_probe_end();
    return NULL;
}
static void *transition(void *unused) {
    (void)unused;
    while(!kim_ble_resource_begin()) sched_yield();
    atomic_store(&acquired,true);
    while(!atomic_load(&release_probe)) sched_yield();
    kim_ble_resource_end();
    return NULL;
}
int main(void) {
    pthread_t p,b;
    assert(pthread_create(&p,NULL,probe,NULL)==0);
    while(!atomic_load(&inflight)) sched_yield();
    assert(pthread_create(&b,NULL,transition,NULL)==0);
    assert(!kim_ble_resource_begin());
    assert(!atomic_load(&acquired));
    atomic_store(&release_probe,true);
    pthread_join(p,NULL);pthread_join(b,NULL);
    assert(atomic_load(&acquired));
    assert(kim_ble_resource_begin());
    assert(!kim_tls_probe_begin());
    kim_ble_resource_end();
    assert(kim_tls_probe_begin()); /* steady BLE window still admits TLS */
    assert(!kim_ble_resource_begin());
    kim_tls_probe_end();
    assert(kim_ble_resource_begin());kim_ble_resource_end();
    return 0;
}
