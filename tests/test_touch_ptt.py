import subprocess
import tempfile
import unittest
from pathlib import Path

HEADER = Path(__file__).resolve().parents[1] / 'firmware/main/touch_ptt.h'

class TouchPTTTest(unittest.TestCase):
    def test_hold_release_loss_mute_and_new_press(self):
        source = r'''
#include <assert.h>
#include <stddef.h>
#include "touch_ptt.h"
int main(void) {
    kim_touch_ptt_t p = {0};
    unsigned turn = 0;
    assert(!kim_touch_recording(&p, turn)); // no automatic microphone
    kim_touch_press(&p); turn = atomic_load(&p.epoch);
    assert(kim_touch_recording(&p, turn));
    assert(!kim_touch_submit(&p, turn, 3200, false, true));
    kim_touch_release(&p);
    assert(kim_touch_submit(&p, turn, 3200, false, true));
    assert(!kim_touch_cancelled(&p, turn)); // release is not cancellation
    assert(!kim_touch_submit(&p, turn, 3199, false, true));
    assert(!kim_touch_submit(&p, turn, 3200, true, true));
    assert(!kim_touch_submit(&p, turn, 3200, false, false));
    kim_touch_press(&p);
    assert(kim_touch_cancelled(&p, turn));
    unsigned next = atomic_load(&p.epoch);
    kim_touch_lost(&p);
    assert(!kim_touch_submit(&p, next, 3200, false, true));
    kim_touch_press(&p); next = atomic_load(&p.epoch);
    assert(kim_touch_recording(&p, next));
    kim_touch_release(&p);
    assert(kim_touch_submit(&p, next, 3200, false, true));
    return 0;
}'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'ptt.c'
            path.write_text(source)
            exe = Path(d) / 'ptt'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(HEADER.parent), str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()
