"""Verify the boot fallback is exactly the authored 4000-ms sleep_wake frame."""
import io
import re
from pathlib import Path

from PIL import Image
import pytest

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'firmware/main/generated'
pytestmark = pytest.mark.skipif('"pixel_avatar.c"' in (ROOT / 'firmware/main/CMakeLists.txt').read_text(),
    reason='authored-video boot test replaced by pixel offline boot; video asset retained as rollback')


def test_sleep_hold_matches_authored_clip_and_boot_source():
    header = (GEN / 'avatar.h').read_text()
    frames = re.findall(r'\{(\d+), (\d+)\}', header.split('kim_avatar_frames[]')[1].split('};')[0])
    clips = re.findall(r'\{(\d+), (\d+), (\d+)\}', header.split('kim_avatar_clips[]')[1])
    start, count, duration = map(int, clips[6])
    frame = start + (4000 * count) // duration
    offset, size = map(int, frames[frame])
    image = Image.open(io.BytesIO((GEN / 'avatar.bin').read_bytes()[offset:offset + size])).convert('RGB')
    assert image.size == (192, 192)
    expected = bytearray()
    for r, g, b in image.get_flattened_data():
        color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        expected.extend((color & 255, color >> 8))
    assert len(expected) == 192 * 192 * 2
    assert (GEN / 'sleep_hold.bin').read_bytes() == expected
    main = (ROOT / 'firmware/main/main.c').read_text()
    assert main.index('memcpy(pixels, sleep_hold_start') < main.index('descriptor.data = (const uint8_t *)pixels')
    assert main.index('descriptor.data = (const uint8_t *)pixels') < main.index('lv_image_set_src(image, &descriptor)')
