"""Compile and execute actual C player bounds/state code with sanitizers."""
import os
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

def test_avatar_player_actual_c(tmp_path):
    binary=tmp_path/'avatar-player'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
        '-DKIM_AVATAR_HOST_TEST','-I',str(ROOT/'firmware/main'),
        str(ROOT/'tests/avatar_player_host.c'),str(ROOT/'firmware/main/avatar_player.c'),
        '-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
