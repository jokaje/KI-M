"""Both reply paths configure the codec before writes; speaking follows queued PCM."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AudioCodecOrderTest(unittest.TestCase):
    def test_both_reply_paths_configure_audible_codec_before_write(self):
        voice = (ROOT / 'firmware/main/voice.c').read_text()
        self.assertIn('esp_codec_dev_set_out_vol(speaker, kim_settings_volume())', voice)
        self.assertIn('esp_codec_dev_set_out_mute(speaker, false)', voice)
        body = voice.split('static bool play_reply(', 1)[1]
        self.assertIn('open_speaker()', body.split('\n}\n', 1)[0])
        self.assertLess(body.index('open_speaker()'),
                        body.index('esp_codec_dev_write(speaker,'))
        stream_sink = voice.split('static bool stream_sink(', 1)[1].split('static stream_result_t', 1)[0]
        self.assertIn('open_speaker()', stream_sink)
        self.assertLess(stream_sink.index('open_speaker()'),
                        stream_sink.index('esp_codec_dev_write(speaker,'))
        helper = voice.split('static bool open_speaker(', 1)[1].split('\n}\n', 1)[0]
        for call in ('esp_codec_dev_open(speaker,',
                     'esp_codec_dev_set_out_vol(speaker, kim_settings_volume())',
                     'esp_codec_dev_set_out_mute(speaker, false)'):
            self.assertIn(call, helper)
        self.assertLess(helper.index('esp_codec_dev_open(speaker,'),
                        helper.index('esp_codec_dev_set_out_vol(speaker, kim_settings_volume())'))
        self.assertLess(helper.index('esp_codec_dev_set_out_vol(speaker, kim_settings_volume())'),
                        helper.index('esp_codec_dev_set_out_mute(speaker, false)'))
        self.assertIn('esp_codec_dev_close(speaker)', helper)
        self.assertNotIn('gpio_set_level(BSP_BUTTON_MUTE_IO', voice)


if __name__ == '__main__':
    unittest.main()
