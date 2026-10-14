"""Production pixel compositor and opt-in wire parser compiled on the host."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_pixel_avatar_production_c():
    with tempfile.TemporaryDirectory() as tmp:
        binary = pathlib.Path(tmp) / 'pixel'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'firmware/main'),
                        str(ROOT / 'tests/pixel_avatar_host.c'),
                        str(ROOT / 'firmware/main/pixel_avatar.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_stream_metadata_split_and_legacy_c():
    with tempfile.TemporaryDirectory() as tmp:
        source = pathlib.Path(tmp) / 'protocol.c'
        source.write_text('''#include <assert.h>
#include "pcm_stream.h"
static bool sink(void *c, const uint8_t *p, size_t n) { (void)p; *(size_t *)c += n; return true; }
int main(void) {
  kim_pcm_stream_t s; size_t used=0; kim_pcm_begin(&s);
  const uint8_t m[]={ 'M',1,0,0,0,1,'A',2,0,0,0,7,8,'E',0,0,0,0 };
  for(size_t i=0;i<sizeof(m);i++) assert(kim_pcm_feed(&s,m+i,1,sink,&used));
  assert(s.emotion==1 && used==2 && kim_pcm_finish(&s));
  kim_pcm_begin(&s); used=0;
  assert(kim_pcm_feed(&s,m+6,sizeof(m)-6,sink,&used));
  assert(s.emotion==0 && used==2 && kim_pcm_finish(&s));
  kim_pcm_begin(&s);
  const uint8_t bad[]={ 'M',1,0,0,0,9 };
  assert(!kim_pcm_feed(&s,bad,sizeof(bad),sink,&used));
  return 0;
}''')
        binary = pathlib.Path(tmp) / 'protocol'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'firmware/main'), str(source),
                        str(ROOT / 'firmware/main/pcm_stream.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_production_composition_renders_in_managed_lvgl(tmp_path):
    """Exercise C compositor -> actual LVGL descriptor -> visible 320x240 pixels."""
    generated = ROOT / 'firmware/main/generated'
    composer = tmp_path / 'composer'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I',
                    str(ROOT / 'firmware/main'), str(ROOT / 'tests/pixel_render_host.c'),
                    str(ROOT / 'firmware/main/pixel_avatar.c'), '-o', str(composer)], check=True)
    raw = tmp_path / 'composed.bin'
    subprocess.run([str(composer), str(generated / 'sprites.bin'),
                    str(generated / 'sprites_mask.bin'), str(raw)], check=True)
    build = tmp_path / 'lvgl'
    subprocess.run(['cmake', '-S', str(ROOT / 'tests/menu_host'), '-B', str(build),
                    '-DCMAKE_BUILD_TYPE=Release'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', str(build), '--target', 'avatar_render_probe', '-j8'],
                   check=True, stdout=subprocess.DEVNULL)
    output = tmp_path / 'screen.ppm'
    subprocess.run([str(build / 'avatar_render_probe'), str(raw), str(output), 'KI-M | Offline'], check=True)
    from PIL import Image
    im = Image.open(output).convert('RGB')
    assert im.size == (320, 240)
    assert im.getpixel((64, 0)) != im.getpixel((160, 120))
    assert len(set(im.crop((64, 0, 256, 192)).get_flattened_data())) > 30

