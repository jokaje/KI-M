import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/ki-m-sprites-v1'
STATES = ('neutral', 'idle', 'blink', 'listening', 'thinking', 'speaking', 'happy', 'offline', 'sleep')


class FirmwareAssetsTest(unittest.TestCase):
    def test_generated_blob_matches_every_original_frame(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            subprocess.run([sys.executable, str(ROOT / 'tools/make_firmware_sprites.py'), str(ASSETS), str(out)], check=True)
            manifest = json.loads((out / 'sprites.json').read_text())
            blob = (out / 'sprites.bin').read_bytes()
            self.assertEqual(list(manifest), list(STATES))
            self.assertEqual(sum(len(v) for v in manifest.values()), 46)
            self.assertEqual(len(blob), 46 * 64 * 64 * 3)
            for state, offsets in manifest.items():
                for n, offset in enumerate(offsets):
                    rgba = Image.open(ASSETS / state / f'frame_{n:03}.png').convert('RGBA')
                    self.assertEqual(rgba.size, (64, 64))
                    self.assertEqual(offset % (64 * 64 * 3), 0)
                    raw = blob[offset:offset + 64 * 64 * 3]
                    for pixel, (r, g, b, a) in enumerate(rgba.get_flattened_data()):
                        color, alpha = struct.unpack_from('<HB', raw, pixel * 3)
                        self.assertEqual(color, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                        self.assertEqual(alpha, a)

    def test_no_muse_runtime_or_tunnel_in_build_sources(self):
        main = (ROOT / 'firmware/main/main.c').read_text()
        voice = (ROOT / 'firmware/main/voice.c').read_text()
        cmake = (ROOT / 'firmware/main/CMakeLists.txt').read_text()
        active = '\n'.join(line for line in (main + voice).splitlines() if not line.lstrip().startswith(('/*', '*', '//')))
        for forbidden in ('muse_glue', 'noise_', 'ble_server', 'jollybot', 'lan_tunnel'):
            self.assertNotIn(forbidden, active.lower() + cmake.lower())
        self.assertIn('.cert_pem = KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL', voice)
        self.assertIn('esp_crt_bundle_attach', voice)
        self.assertIn('strncmp(KIM_SERVER_URL, "https://", 8)', voice)
        self.assertIn('gpio_get_level(BSP_BUTTON_MUTE_IO)', voice)
        self.assertIn('box_3_microphone.c', cmake)

    def test_both_reply_paths_configure_audible_codec_before_write(self):
        """Delegated to test_audio_codec_order: both paths configure the codec
        before writes; the actual voice.c write test verifies display timing."""
        from test_audio_codec_order import AudioCodecOrderTest
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(AudioCodecOrderTest)
        result = unittest.TextTestRunner(stream=None).run(suite)
        self.assertTrue(result.wasSuccessful())

    def test_voice_input_keeps_full_screen_touch_and_gpio_out_of_voice_task(self):
        main = (ROOT / 'firmware/main/main.c').read_text()
        voice = (ROOT / 'firmware/main/voice.c').read_text()
        self.assertIn('lv_obj_set_size(touch, lv_pct(100), lv_pct(100))', main)
        for event in ('LV_EVENT_PRESSED', 'LV_EVENT_RELEASED', 'LV_EVENT_PRESS_LOST'):
            self.assertIn(event, main)
        self.assertIn('kim_touch_submit(', voice)
        self.assertIn('kim_touch_recording(', voice)
        self.assertNotIn('gpio_get_level(BSP_BUTTON_CONFIG_IO)', voice)
        self.assertIn('Hold screen or BOOT', main)


if __name__ == '__main__':
    unittest.main()
