"""Guard the real main.c LVGL handoff against bypassing the player validity gate."""
from pathlib import Path
import pytest

MAIN = (Path(__file__).resolve().parents[1] / 'firmware/main/main.c').read_text()
pytestmark = pytest.mark.skipif('"pixel_avatar.c"' in (Path(__file__).resolve().parents[1] / 'firmware/main/CMakeLists.txt').read_text(),
    reason='video main handoff superseded by the requested pixel renderer; rollback source retained')


def test_main_uses_validated_source_and_retires_before_release():
    loop = MAIN.split('/* Voice/menu/sleep state resolves on every iteration', 1)[1].split('bsp_display_unlock();', 1)[0]
    assert 'kim_avatar_request(&avatar, clip, elapsed, video_epoch)' in loop
    assert 'kim_avatar_display_data(&avatar, pixels)' in loop
    assert 'render(state, frame)' not in loop
    assert 'kim_avatar_select_clip(state, kim_menu_emotion(), sleep_phase)' in loop
    assert 'kim_avatar_elapsed(clip, sleep_phase, sleep_elapsed, now_ms, clip_started)' in loop
    assert 'descriptor.data = source;' in loop
    assert loop.index('lv_image_set_src(image, &descriptor)') < loop.index('kim_avatar_release(&avatar)')
    assert 'descriptor.data = (const uint8_t *)avatar.front' not in loop
    assert 'previous_sleep_phase != sleep_phase' in loop
    assert 'previous_state != state' in loop
    assert '++tick % 10 == 0' not in loop  # per-iteration handoff, no 100-ms grid
    # Boot initializes an authored sleep pose before first LVGL source;
    # pixel-art and uninitialized PSRAM are both forbidden.
    boot = MAIN.split('descriptor.data = (const uint8_t *)pixels;', 1)[1].split('image = lv_image_create(screen);', 1)[0]
    assert 'render(KIM_IDLE, 0)' not in boot
    assert 'atomic_store(&avatar.neutral_valid, true)' not in boot
    assert 'memcpy(avatar.neutral, pixels' not in MAIN
    assert 'kim_avatar_publish_idle_bridge(&avatar)' not in loop
    assert 'memcpy(pixels, sleep_hold_start' in MAIN
    assert 'generated/sleep_hold.bin' in (Path(__file__).resolve().parents[1] / 'firmware/main/CMakeLists.txt').read_text()


def test_unchanged_video_does_not_invalidate_every_poll():
    loop = MAIN.split('/* Voice/menu/sleep state resolves on every iteration', 1)[1].split('bsp_display_unlock();', 1)[0]
    assert 'lv_obj_invalidate(image);' not in loop
    assert 'source == (const uint8_t *)pixels' not in loop.split('if (changed || descriptor.data != source', 1)[1].split('lv_image_set_src', 1)[0]
    assert 'offline_frame' not in loop


def test_perf_probe_reports_decode_and_handoff_and_heap():
    player = (Path(__file__).resolve().parents[1] / 'firmware/main/avatar_player.c').read_text()
    assert 'esp_timer_get_time()' in player
    assert 'decode_us_max' in player
    assert 'heap_caps_get_largest_free_block' in MAIN
    assert 'handoff_us_max' in MAIN
