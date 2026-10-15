"""Compile and exercise the production state policy/parser C without ESP hardware."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware/main'


def test_character_state_actual_c():
    with tempfile.TemporaryDirectory() as directory:
        executable = pathlib.Path(directory) / 'state-test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(MAIN),
                        str(ROOT / 'tests/character_state_harness.c'),
                        str(MAIN / 'kim_character_state.c'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
