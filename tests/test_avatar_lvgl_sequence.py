"""Execute timed production C player handoffs through the managed LVGL 9 renderer."""
import subprocess
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif('"pixel_avatar.c"' in (ROOT / 'firmware/main/CMakeLists.txt').read_text(),
    reason='video main handoff superseded by the requested pixel renderer; rollback source retained')


def test_avatar_timed_lvgl_sequence(tmp_path):
    build = tmp_path / 'lvgl-sequence'
    build.mkdir()
    main = (ROOT / 'firmware/main/main.c').read_text()
    boot = main.split('    memcpy(pixels, sleep_hold_start,', 1)[1].split(
        '    lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);', 1)[0]
    (build / 'avatar_main_boot.inc').write_text(
        '    memcpy(pixels, sleep_hold_start,' + boot +
        '    lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);\n')
    subprocess.run(['cmake', '-S', str(ROOT / 'tests/menu_host'), '-B', str(build),
                    '-DCMAKE_BUILD_TYPE=Release'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', str(build), '--target', 'avatar_sequence_probe', '-j8'],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(build / 'avatar_sequence_probe')], check=True)
