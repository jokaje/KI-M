import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER_DIR = ROOT / 'firmware/main'
SCRATCH = Path.home() / '.hermes/cache/scratch'


class BootPTTTest(unittest.TestCase):
    def test_debounced_boot_and_single_owner_turns(self):
        source = r'''
#include <assert.h>
#include "touch_ptt.h"
#define E() atomic_load(&p.epoch)
#define H() atomic_load(&p.held)
#define SAMPLE(low, time) kim_boot_sample(&p, (low), (time))
int main(void) {
    kim_touch_ptt_t p = {0};
    /* BOOT held at startup cannot start capture until released and repressed. */
    SAMPLE(true, 0); SAMPLE(true, 100); assert(E() == 0 && !H());
    SAMPLE(false, 101); SAMPLE(false, 130); assert(E() == 0);
    SAMPLE(false, 131); assert(E() == 0); /* arm, not a press */
    SAMPLE(true, 140); SAMPLE(false, 150); SAMPLE(false, 180); assert(E() == 0);
    SAMPLE(true, 190); SAMPLE(true, 219); assert(E() == 0);
    SAMPLE(true, 220); unsigned boot = E(); assert(boot == 1 && kim_touch_recording(&p, boot));
    kim_touch_press(&p); assert(E() == boot); /* touch cannot steal BOOT */
    kim_touch_release(&p); kim_touch_lost(&p); assert(H() && !kim_touch_cancelled(&p, boot));
    SAMPLE(false, 225); SAMPLE(true, 230); assert(H()); /* release bounce */
    SAMPLE(false, 240); SAMPLE(false, 270); assert(!H());
    assert(kim_touch_submit(&p, boot, 3200, false, true));
    assert(!kim_touch_cancelled(&p, boot));
    SAMPLE(true, 280); SAMPLE(true, 310); assert(E() == boot + 1);
    assert(kim_touch_cancelled(&p, boot)); /* deliberate new press cancels reply */
    unsigned next = E();
    SAMPLE(false, 320); SAMPLE(false, 350);
    assert(kim_touch_submit(&p, next, 3200, false, true));
    kim_touch_press(&p); unsigned touch = E(); assert(touch == next + 1);
    assert(kim_touch_recording(&p, touch));
    SAMPLE(true, 360); SAMPLE(true, 390); assert(E() == touch); /* BOOT nonowner */
    SAMPLE(false, 400); SAMPLE(false, 430); assert(H());
    kim_touch_lost(&p); assert(kim_touch_cancelled(&p, touch));
    assert(!kim_touch_submit(&p, touch, 3200, false, true));
    kim_touch_press(&p); touch = E(); assert(kim_touch_recording(&p, touch));
    kim_touch_release(&p); assert(kim_touch_submit(&p, touch, 3200, false, true));
    /* A BOOT press ignored during a touch hold must not start on touch release. */
    kim_touch_press(&p); touch = E();
    SAMPLE(true, 440); SAMPLE(true, 470); assert(E() == touch);
    kim_touch_release(&p); assert(!H() && E() == touch);
    SAMPLE(false, 480); SAMPLE(false, 510); assert(E() == touch);
    SAMPLE(true, 520); SAMPLE(true, 550); assert(E() == touch + 1);
    return 0;
}'''
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            path = Path(tmp) / 'boot.c'
            path.write_text(source)
            exe = Path(tmp) / 'boot'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(HEADER_DIR), str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_boot_is_polled_with_lvgl_lock_not_voice(self):
        main = (HEADER_DIR / 'main.c').read_text()
        voice = (HEADER_DIR / 'voice.c').read_text()
        self.assertIn('gpio_config(', main)
        self.assertIn('GPIO_NUM_0', main)
        self.assertIn('kim_boot_sample(&kim_touch_ptt,', main)
        self.assertNotIn('kim_boot_sample(', voice)
        locked = main.split('if (bsp_display_lock(1000)) {', 1)[1].split('bsp_display_unlock();', 1)[0]
        self.assertIn('kim_boot_sample(&kim_touch_ptt,', locked)
        self.assertRegex((ROOT / 'firmware/CMakeLists.txt').read_text(),
                         r'project\(ki-m-box3 VERSION 0\.[4-9]\.\d+\)')


if __name__ == '__main__':
    unittest.main()
