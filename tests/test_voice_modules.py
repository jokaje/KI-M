import subprocess
from pathlib import Path

def test_audio_modules_host(tmp_path):
    root = Path(__file__).resolve().parents[1] / 'firmware/main'
    source = tmp_path/'probe.c'
    source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "audio_jitter.h"
#include "voice_activity.h"
int main(void) {
  kim_jitter_t j; kim_jitter_reset(&j);
  unsigned char in[640], out[640]; memset(in, 0x42, sizeof(in));
  for(int i=0;i<20;i++) assert(kim_jitter_push(&j,in,640)==640);
  for(int i=0;i<20;i++){assert(kim_jitter_pop(&j,out,640)==640);assert(!memcmp(in,out,640));}
  assert(!kim_jitter_pop(&j,out,640) && j.underruns==1);
  kim_vad_t v; kim_vad_reset(&v);
  int16_t quiet[160]={0}, loud[160]; for(int i=0;i<160;i++)loud[i]=i%2?9000:-9000;
  for(int i=0;i<10;i++) assert(!kim_vad_feed(&v,quiet,160));
  for(int i=0;i<3;i++)kim_vad_feed(&v,loud,160);
  assert(v.speech);
  for(int i=0;i<45;i++)kim_vad_feed(&v,quiet,160);
  assert(!v.speech);
  return 0;
}
''')
    binary=tmp_path/'probe'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(root),str(source),str(root/'audio_jitter.c'),str(root/'voice_activity.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
