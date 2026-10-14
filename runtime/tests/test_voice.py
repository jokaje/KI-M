import io
import struct
import wave

import pytest

from voice import SentenceSplitter, decode_tts_wav, strip_thinking


def test_splitter_preserves_partial_and_decimal():
    split = SentenceSplitter()
    assert split.feed('Hallo. Noch 3.') == ['Hallo.']
    assert split.feed('14 Punkte! Weiter') == ['Noch 3.14 Punkte!']
    assert split.finish() == ['Weiter']


def test_thinking_never_spoken():
    split = SentenceSplitter()
    for token in ['<think>', 'private', '</think>', ' Guten Tag. ']:
        result = split.feed(token)
    assert result == ['Guten Tag.']
    assert strip_thinking('<think>secret</think>Hallo') == 'Hallo'


def test_reject_malformed_wave():
    with pytest.raises(ValueError):
        decode_tts_wav(b'RIFF' + b'\0' * 100)
    with io.BytesIO() as out:
        with wave.open(out, 'wb') as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(24000)
            wav.writeframes(struct.pack('<h', 5000) * 24000)
        assert any(decode_tts_wav(out.getvalue()))
