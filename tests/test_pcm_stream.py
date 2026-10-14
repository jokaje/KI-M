"""Host regressions for backend STREAM_PROTOCOL.md v1 framing and cancellation."""
import ctypes
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

class Decoder(ctypes.Structure):
    _fields_ = [('header', ctypes.c_uint8 * 5), ('header_used', ctypes.c_size_t),
                ('remaining', ctypes.c_size_t), ('total_bytes', ctypes.c_size_t),
                ('frames', ctypes.c_uint), ('pending', ctypes.c_uint8),
                ('has_pending', ctypes.c_bool), ('ended', ctypes.c_bool), ('failed', ctypes.c_bool)]
CALLBACK = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t)

def frame(kind, data=b''):
    return kind.encode() + struct.pack('<I', len(data)) + data

class PCMStreamTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        library = Path(cls.temp.name) / 'libpcm.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-o', str(library), str(ROOT / 'firmware/main/pcm_stream.c')], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.kim_pcm_begin.argtypes = [ctypes.POINTER(Decoder)]
        cls.lib.kim_pcm_feed.argtypes = [ctypes.POINTER(Decoder), ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, CALLBACK, ctypes.c_void_p]
        cls.lib.kim_pcm_feed.restype = ctypes.c_bool
        cls.lib.kim_pcm_finish.argtypes = [ctypes.POINTER(Decoder)]
        cls.lib.kim_pcm_finish.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def decode(self, pieces, cancel_at=None):
        d = Decoder()
        self.lib.kim_pcm_begin(ctypes.byref(d))
        heard = bytearray()
        @CALLBACK
        def sink(ctx, data, size):
            if cancel_at is not None and len(heard) >= cancel_at:
                return False
            heard.extend(ctypes.string_at(data, size))
            return True
        ok = True
        for part in pieces:
            buf = (ctypes.c_uint8 * len(part)).from_buffer_copy(part)
            if not self.lib.kim_pcm_feed(ctypes.byref(d), buf, len(part), sink, None):
                ok = False
                break
        return ok and self.lib.kim_pcm_finish(ctypes.byref(d)), bytes(heard), d

    def test_short_reads_and_odd_bytes_play_before_end(self):
        raw = frame('A', b'\x01\x02\x03\x04\x05\x06') + frame('E')
        ok, heard, _ = self.decode([bytes([n]) for n in raw])
        self.assertTrue(ok)
        self.assertEqual(heard, b'\x01\x02\x03\x04\x05\x06')

    def test_first_chunk_reaches_speaker_before_terminal(self):
        d = Decoder()
        self.lib.kim_pcm_begin(ctypes.byref(d))
        heard = bytearray()
        @CALLBACK
        def sink(ctx, data, size):
            heard.extend(ctypes.string_at(data, size))
            return True
        first = frame('A', b'\x01\x02')
        buf = (ctypes.c_uint8 * len(first)).from_buffer_copy(first)
        self.assertTrue(self.lib.kim_pcm_feed(ctypes.byref(d), buf, len(buf), sink, None))
        self.assertEqual(heard, b'\x01\x02')
        self.assertFalse(self.lib.kim_pcm_finish(ctypes.byref(d)))

    def test_two_sentences_and_terminal(self):
        ok, heard, d = self.decode([frame('A', b'\x01\x02') + frame('A', b'\x03\x04') + frame('E')])
        self.assertTrue(ok)
        self.assertEqual(heard, b'\x01\x02\x03\x04')
        self.assertEqual(d.frames, 2)

    def test_empty_success_is_not_audible_reply(self):
        self.assertFalse(self.decode([frame('E')])[0])

    def test_truncated_or_extra_bytes_rejected(self):
        self.assertFalse(self.decode([frame('A', b'\x01\x02')])[0])
        self.assertFalse(self.decode([frame('A', b'\x01\x02')[:-1]])[0])
        self.assertFalse(self.decode([frame('E') + b'x'])[0])

    def test_malformed_frames_rejected(self):
        for wire in (frame('Z'), frame('A'), frame('A', b'\x00'),
                     b'A' + struct.pack('<I', 256002), frame('X'),
                     frame('E') + frame('A', b'\x01\x02')):
            with self.subTest(wire=wire[:5]):
                self.assertFalse(self.decode([wire])[0])

    def test_cancellation_stops_delivery(self):
        wire = frame('A', b'\x01\x02\x03\x04') + frame('E')
        ok, heard, _ = self.decode([wire[:7], wire[7:]], cancel_at=2)
        self.assertFalse(ok)
        self.assertEqual(heard, b'\x01\x02')

    def test_frame_count_and_aggregate_bound(self):
        self.assertFalse(self.decode([frame('A', b'\x00\x00') * 17 + frame('E')])[0])
        self.assertFalse(self.decode([frame('A', b'\x00' * 256000) * 6 + frame('E')])[0])

if __name__ == '__main__':
    unittest.main()
