"""Compile and exercise the firmware playback clock as C, at the 100-ms UI cadence."""
import ctypes
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"


class PlaybackClockTest(unittest.TestCase):
    def test_repeated_pcm_writes_and_stream_gaps(self):
        with tempfile.TemporaryDirectory() as td:
            lib = pathlib.Path(td) / "playback.so"
            subprocess.run(["cc", "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                            "-I", str(MAIN), str(MAIN / "playback_clock.c"),
                            "-o", str(lib)], check=True)
            c = ctypes.CDLL(str(lib))
            class Clock(ctypes.Structure):
                _fields_ = [("until_us", ctypes.c_int64), ("level", ctypes.c_uint)]
            c.kim_playback_reset.argtypes = [ctypes.POINTER(Clock)]
            c.kim_playback_written.argtypes = [ctypes.POINTER(Clock), ctypes.c_int64, ctypes.c_uint, ctypes.c_uint]
            c.kim_playback_audible.argtypes = [ctypes.POINTER(Clock), ctypes.c_int64]
            c.kim_playback_audible.restype = ctypes.c_bool
            c.kim_playback_level.argtypes = [ctypes.POINTER(Clock), ctypes.c_int64]
            c.kim_playback_level.restype = ctypes.c_uint
            c.kim_playback_remaining.argtypes = [ctypes.POINTER(Clock), ctypes.c_int64]
            c.kim_playback_remaining.restype = ctypes.c_int64
            clock = Clock()
            c.kim_playback_reset(ctypes.byref(clock))
            # Two 640-byte codec writes (20 ms each), with a UI poll during
            # the frame. At frame return PCM remains in DMA, not THINKING.
            c.kim_playback_written(ctypes.byref(clock), 1000, 640, 2)
            c.kim_playback_written(ctypes.byref(clock), 3000, 640, 1)
            self.assertTrue(c.kim_playback_audible(ctypes.byref(clock), 20000))
            self.assertTrue(c.kim_playback_audible(ctypes.byref(clock), 40000))
            self.assertEqual(c.kim_playback_level(ctypes.byref(clock), 40000), 1)
            self.assertFalse(c.kim_playback_audible(ctypes.byref(clock), 100000))
            # Streaming TTS stalls: no indefinite SPEAKING after the queued
            # PCM runs out. The next frame resumes without network sleeping.
            c.kim_playback_written(ctypes.byref(clock), 500000, 640, 2)
            self.assertTrue(c.kim_playback_audible(ctypes.byref(clock), 510000))
            self.assertFalse(c.kim_playback_audible(ctypes.byref(clock), 700000))
            c.kim_playback_reset(ctypes.byref(clock))  # cancellation/mute/failure
            self.assertFalse(c.kim_playback_audible(ctypes.byref(clock), 510000))
            # A single slow write cannot create a phantom full-frame tail.
            c.kim_playback_written(ctypes.byref(clock), 900000, 640, 2)
            self.assertLessEqual(c.kim_playback_remaining(ctypes.byref(clock), 900000), 90000)


if __name__ == "__main__":
    unittest.main()
