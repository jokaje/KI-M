import ctypes
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'firmware/main'


class Trial(ctypes.Structure):
    _fields_ = [('pending', ctypes.c_bool), ('captured', ctypes.c_bool),
                ('replied', ctypes.c_bool), ('menu_opened', ctypes.c_bool),
                ('menu_cycle', ctypes.c_bool), ('started_ms', ctypes.c_uint32)]


def test_ota_trial_requires_real_turn_menu_cycle_and_deadline():
    with tempfile.TemporaryDirectory() as d:
        lib = Path(d) / 'trial.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Werror',
                        '-I', str(ROOT), str(ROOT / 'kim_trial.c'), '-o', str(lib)], check=True)
        c = ctypes.CDLL(str(lib))
        c.kim_trial_start.argtypes = [ctypes.POINTER(Trial), ctypes.c_bool, ctypes.c_uint32]
        c.kim_trial_capture.argtypes = [ctypes.POINTER(Trial), ctypes.c_bool]
        c.kim_trial_reply.argtypes = [ctypes.POINTER(Trial), ctypes.c_bool, ctypes.c_bool]
        c.kim_trial_menu.argtypes = [ctypes.POINTER(Trial), ctypes.c_bool]
        c.kim_trial_ready.argtypes = [ctypes.POINTER(Trial), ctypes.c_bool, ctypes.c_bool, ctypes.c_bool, ctypes.c_bool]
        c.kim_trial_ready.restype = ctypes.c_bool
        c.kim_trial_expired.argtypes = [ctypes.POINTER(Trial), ctypes.c_uint32]
        c.kim_trial_expired.restype = ctypes.c_bool
        t = Trial()
        c.kim_trial_start(ctypes.byref(t), False, 100)
        assert not c.kim_trial_ready(ctypes.byref(t), True, True, True, True)
        assert not c.kim_trial_expired(ctypes.byref(t), 180101)
        c.kim_trial_start(ctypes.byref(t), True, 0xfffffff0)
        ready = lambda: c.kim_trial_ready(ctypes.byref(t), True, True, True, True)
        assert not ready()
        c.kim_trial_capture(ctypes.byref(t), False)
        c.kim_trial_reply(ctypes.byref(t), True, True)
        c.kim_trial_menu(ctypes.byref(t), True)
        c.kim_trial_menu(ctypes.byref(t), False)
        assert not ready()
        c.kim_trial_capture(ctypes.byref(t), True)
        c.kim_trial_reply(ctypes.byref(t), False, True)
        assert not ready()
        c.kim_trial_reply(ctypes.byref(t), True, False)
        assert not ready()
        c.kim_trial_reply(ctypes.byref(t), True, True)
        assert ready()
        assert not c.kim_trial_ready(ctypes.byref(t), False, True, True, True)
        assert not c.kim_trial_expired(ctypes.byref(t), (0xfffffff0 + 179999) & 0xffffffff)
        assert c.kim_trial_expired(ctypes.byref(t), (0xfffffff0 + 180000) & 0xffffffff)
