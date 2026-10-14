import subprocess
from pathlib import Path

def _build(tmp_path):
    root = Path(__file__).resolve().parents[1] / 'firmware/main'
    src = tmp_path / 'probe.c'
    src.write_text(r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "handsfree.h"

static uint8_t loud[320], quiet[320];
static void mkframe(uint8_t *f, int amp) {
  for (int i = 0; i < 160; ++i) {
    int16_t s = (int16_t)(i % 2 ? amp : -amp);
    f[2*i] = (uint8_t)(s & 0xff); f[2*i+1] = (uint8_t)((s >> 8) & 0xff);
  }
}

int main(void) {
  mkframe(loud, 9000); memset(quiet, 0, sizeof quiet);
  uint8_t buf[1024 * 320];
  kim_handsfree_t h; kim_handsfree_init(&h, buf, sizeof buf);

  /* 1) 500 ms of quiet must not complete anything. */
  for (int i = 0; i < 50; ++i) assert(!kim_handsfree_frame(&h, quiet));
  assert(kim_handsfree_state(&h) == KIM_HF_IDLE);

  /* 2) Re-init discards old silent pre-roll and re-arms. */
  kim_handsfree_init(&h, buf, sizeof buf);
  assert(kim_handsfree_state(&h) == KIM_HF_IDLE && kim_handsfree_length(&h) == 0);

  /* 3) Speech endpoint: min voiced gate, pre-roll keeps the onset audio.
   *    VAD needs 3 active frames; the transition frame rides into the
   *    pre-roll ring and is then delivered once, in order. */
  for (int i = 0; i < 10; ++i) assert(!kim_handsfree_frame(&h, quiet));
  assert(!kim_handsfree_frame(&h, loud));  /* active=1: still listening */
  assert(!kim_handsfree_frame(&h, loud));  /* active=2 */
  (void)kim_handsfree_frame(&h, loud);     /* active=3: VAD says speech */
  assert(kim_handsfree_state(&h) == KIM_HF_SPEECH); /* capture phase, not done */
  for (int i = 0; i < 8; ++i) assert(!kim_handsfree_frame(&h, loud));  /* voiced grows */
  for (int i = 0; i < KIM_HF_END_SILENCE - 1; ++i) assert(!kim_handsfree_frame(&h, quiet));
  bool done = kim_handsfree_frame(&h, quiet);
  assert(done && kim_handsfree_state(&h) == KIM_HF_COMPLETE);
  assert(kim_handsfree_voiced(&h) >= KIM_HF_MIN_VOICED);
  size_t len = kim_handsfree_length(&h);
  /* expected = preroll flush (10 quiet + 3 loud = 13) + 8 loud + 45 quiet */
  assert(len == (13 + 8 + KIM_HF_END_SILENCE) * 320);
  /* First delivered frame is the preserved pre-roll context, not late speech. */
  assert(memcmp(buf, quiet, 320) == 0); /* onset context preserved */

  /* 5) 8-s speech cap measured from speech START: talk fast, endpoint fires even
   *    when trailing silence never reaches 450 ms. */
  kim_handsfree_init(&h, buf, sizeof buf);
  for (int i = 0; i < 20; ++i) assert(!kim_handsfree_frame(&h, quiet));
  for (int i = 0; i < (KIM_HF_MAX_SPEECH_MS / 10) + 5; ++i)
      kim_handsfree_frame(&h, loud);   /* continuous speech past the cap */
  assert(kim_handsfree_state(&h) == KIM_HF_COMPLETE);

  /* 6) Buffer overrun safeguard: a capture too small for the pre-roll
   *    flush drops the turn at speech onset and returns to idle cleanly,
   *    so a tiny buffer can neither corrupt audio nor overrun. */
  uint8_t tiny[320];
  kim_handsfree_init(&h, tiny, sizeof tiny);
  for (int i = 0; i < 5; ++i) assert(!kim_handsfree_frame(&h, quiet));
  (void)kim_handsfree_frame(&h, loud);   /* onset wants 5 preroll frames in */
  assert(kim_handsfree_state(&h) == KIM_HF_IDLE); /* 320 B < preroll -> drop */
  assert(kim_handsfree_length(&h) == 0);

  return 0;
}
''')
    exe = tmp_path / 'probe'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(src), str(root/'handsfree.c'), str(root/'voice_activity.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

def test_handsfree_realtime(tmp_path):
    _build(tmp_path)
