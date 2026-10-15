"""Compile the production voice loop's wait/input scheduling tail, not a retyped model.
A stalled metadata request must not hold the voice worker after a physical input.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VOICE = ROOT / 'firmware/main/voice.c'


def test_stalled_metadata_does_not_serialize_ptt_dispatch():
    source = VOICE.read_text()
    tail = source.split('wait_input:\n', 1)[1].split('\n    }\n}', 1)[0]
    worker = source.split('static void character_task(void *arg) {\n', 1)[1].split('\n}\n', 1)[0]
    # Real production statements are compiled into a two-iteration voice-loop
    # harness. A stalled probe cooperatively receives input from another core.
    harness = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
static atomic_bool held, in_probe, released;
static atomic_int dispatches;
static void character_probe(void) {
    atomic_store(&in_probe, true);
    struct timespec wait = {0, 300000000};
    nanosleep(&wait, NULL); /* stalled TLS read with a 300 ms socket timeout */
}
static void device_state_report(void) {}
typedef struct { int unused; } kim_device_state_t;
static kim_device_state_t device_state;
static int64_t esp_timer_get_time(void) { return 0; }
static bool kim_device_state_due(const kim_device_state_t *s, uint32_t now) { (void)s; (void)now; return false; }
static void vTaskDelay(int ms) { (void)ms; struct timespec t={0,5000000}; nanosleep(&t,NULL); }
#define pdMS_TO_TICKS(x) (x)
static void character_task(void *arg) {
''' + worker + r'''
}
static void *metadata(void *arg) { character_task(arg); return NULL; }
static void *input(void *arg) {
    (void)arg;
    while (!atomic_load(&in_probe)) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
    struct timespec onset={0,2000000}; nanosleep(&onset,NULL);
    atomic_store(&held, true);
    atomic_store(&released, true);
    return NULL;
}
int main(void) {
    pthread_t producer, poller;
    assert(!pthread_create(&poller,NULL,metadata,NULL));
    assert(!pthread_create(&producer,NULL,input,NULL));
    struct timespec started, dispatched;
    clock_gettime(CLOCK_MONOTONIC,&started);
    for (int i=0; i<2; ++i) {
        if (atomic_load(&released)) {
            atomic_fetch_add(&dispatches, 1);
            clock_gettime(CLOCK_MONOTONIC,&dispatched);
        }
wait_input:
''' + tail + r'''
    }
    pthread_join(producer,NULL);
    assert(atomic_load(&dispatches)==1);
    long elapsed_ms=(dispatched.tv_sec-started.tv_sec)*1000+
                    (dispatched.tv_nsec-started.tv_nsec)/1000000;
    printf("PTT dispatch after stalled metadata: %ld ms\n", elapsed_ms);
    assert(elapsed_ms < 200);
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'harness.c'
        binary = Path(tmp) / 'harness'
        path.write_text(harness)
        subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                        '-Wno-unused-label', str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=4)
