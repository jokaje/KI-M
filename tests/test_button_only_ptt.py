"""Compile the actual firmware touch callback: only BOOT may own a turn."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]

def test_actual_screen_callback_never_starts_submits_or_cancels_ptt():
    main=(ROOT/'firmware/main/main.c').read_text()
    callback=main.split('static void touch_event(',1)[1].split('\n}',1)[0]
    source='''#include <assert.h>
#include "touch_ptt.h"
typedef int lv_event_code_t;
typedef struct { int code; } lv_event_t;
enum { LV_EVENT_PRESSED=1, LV_EVENT_RELEASED=2, LV_EVENT_PRESS_LOST=3 };
static lv_event_code_t lv_event_get_code(lv_event_t *e) { return e->code; }
static kim_touch_ptt_t kim_touch_ptt;
static int kim_character_poll;
static void kim_character_interact(int *p) { ++*p; }
static int kim_menu_recording_allowed(void) __attribute__((unused));
static int kim_menu_recording_allowed(void) { return 1; }
static void touch_event('''+callback+'\n}\n'+r'''
static void screen(void) {
 for(int i=1;i<=3;++i) { lv_event_t e={i};touch_event(&e); }
}
int main(void) {
 screen();assert(atomic_load(&kim_touch_ptt.epoch)==0);
 assert(!atomic_load(&kim_touch_ptt.held));
 kim_boot_sample(&kim_touch_ptt,false,0);kim_boot_sample(&kim_touch_ptt,false,30);
 kim_boot_sample(&kim_touch_ptt,true,40);kim_boot_sample(&kim_touch_ptt,true,70);
 unsigned turn=atomic_load(&kim_touch_ptt.epoch);
 assert(turn==1 && kim_touch_recording(&kim_touch_ptt,turn));
 screen();assert(kim_touch_recording(&kim_touch_ptt,turn));
 assert(atomic_load(&kim_touch_ptt.epoch)==turn);
 kim_boot_sample(&kim_touch_ptt,false,80);kim_boot_sample(&kim_touch_ptt,false,110);
 assert(kim_touch_submit(&kim_touch_ptt,turn,3200,false,true));
 screen();assert(!kim_touch_cancelled(&kim_touch_ptt,turn));
 assert(atomic_load(&kim_touch_ptt.epoch)==turn);
 kim_boot_sample(&kim_touch_ptt,true,120);kim_boot_sample(&kim_touch_ptt,true,150);
 assert(kim_touch_cancelled(&kim_touch_ptt,turn));
 assert(atomic_load(&kim_touch_ptt.epoch)==turn+1);
 assert(kim_character_poll==9);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(dir=Path.home()/'.hermes/cache/scratch') as d:
        c=Path(d)/'probe.c';exe=Path(d)/'probe';c.write_text(source)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'firmware/main'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    assert 'Hold screen or BOOT' not in main
    assert 'kim_menu_init(screen, touch);' in main
    assert 'kim_boot_sample(&kim_touch_ptt,' in main
