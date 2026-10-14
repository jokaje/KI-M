import subprocess
import tempfile
import unittest
from pathlib import Path

MAIN = Path(__file__).resolve().parents[1] / 'firmware/main'


class PublicationTest(unittest.TestCase):
    def test_reader_at_epoch_publication_cannot_consume_unheld_press(self):
        source = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
static atomic_bool published, read_done;
static unsigned observe_epoch(atomic_uint *p, unsigned n) {
    unsigned prior = atomic_fetch_add_explicit(p, n, memory_order_seq_cst);
    atomic_store(&published, 1);
    while (!atomic_load(&read_done)) sched_yield();
    return prior;
}
#undef atomic_fetch_add
#define atomic_fetch_add(p, n) observe_epoch(p, n)
#include "touch_ptt.h"
static kim_touch_ptt_t input;
static void *reader(void *unused) {
    (void)unused;
    while (!atomic_load(&published)) sched_yield();
    unsigned turn = atomic_load(&input.epoch);
    assert(turn == 1);
    assert(kim_touch_recording(&input, turn));
    atomic_store(&read_done, 1);
    return 0;
}
int main(void) {
    pthread_t thread;
    assert(pthread_create(&thread, 0, reader, 0) == 0);
    kim_touch_press(&input);
    assert(pthread_join(thread, 0) == 0);
    kim_touch_release(&input);
    assert(kim_touch_submit(&input, 1, 3200, false, true));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            c = Path(directory) / 'publication.c'
            c.write_text(source)
            exe = Path(directory) / 'publication'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I', str(MAIN), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_shared_runtime_flags_are_atomic(self):
        header = (MAIN / 'voice.h').read_text()
        source = (MAIN / 'voice.c').read_text()
        for kind, name in [('atomic_int', 'kim_voice_state'), ('atomic_uint', 'kim_mouth_level'),
                           ('atomic_bool', 'kim_is_online'), ('atomic_bool', 'kim_reply_failed')]:
            self.assertIn(f'extern {kind} {name};', header)
            self.assertIn(f'{kind} {name}', source)


if __name__ == '__main__':
    unittest.main()
