"""Host behavioral tests compile the actual menu gate/state machine (not a rewrite)."""
import ctypes
import pathlib
import subprocess
import tempfile
import unittest

MAIN = pathlib.Path(__file__).resolve().parents[1] / "main"


class MenuGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = pathlib.Path(cls.tmp.name) / "menu.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-DKIM_MENU_GATE_TEST", "-I", str(MAIN),
                        str(MAIN / "kim_menu.c"), "-o", str(lib)], check=True)
        cls.api = ctypes.CDLL(str(lib))
        cls.api.kim_menu_recording_allowed.restype = ctypes.c_bool
        cls.api.kim_menu_open.restype = ctypes.c_bool
        cls.api.kim_menu_gate_test_step.argtypes = [ctypes.c_uint32, ctypes.c_bool, ctypes.c_bool]
        cls.api.kim_menu_gate_test_open.argtypes = [ctypes.c_bool]
        cls.api.kim_menu_gate_test_sleep.argtypes = [ctypes.c_bool]
        cls.api.kim_menu_gate_test_sleep_due.argtypes = [ctypes.c_uint32, ctypes.c_uint32,
            ctypes.c_int, ctypes.c_bool, ctypes.c_bool, ctypes.c_bool]
        cls.api.kim_menu_gate_test_sleep_due.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.api.kim_menu_gate_test_reset()

    def test_menu_blocks_recording_until_release_after_close(self):
        self.api.kim_menu_gate_test_open(True)
        self.assertTrue(self.api.kim_menu_open())
        self.assertFalse(self.api.kim_menu_recording_allowed())
        self.api.kim_menu_gate_test_open(False)
        self.api.kim_menu_gate_test_step(100, True, False)
        self.assertFalse(self.api.kim_menu_recording_allowed())
        self.api.kim_menu_gate_test_step(110, False, False)
        self.assertTrue(self.api.kim_menu_recording_allowed())

    def test_sleep_wake_consumes_touch_and_boot_hold(self):
        self.api.kim_menu_gate_test_sleep(True)
        self.assertFalse(self.api.kim_menu_recording_allowed())
        self.api.kim_menu_gate_test_sleep(False)
        self.api.kim_menu_gate_test_step(100, True, True)
        self.assertFalse(self.api.kim_menu_recording_allowed())
        self.api.kim_menu_gate_test_step(110, False, True)
        self.assertFalse(self.api.kim_menu_recording_allowed())
        self.api.kim_menu_gate_test_step(120, False, False)
        self.assertTrue(self.api.kim_menu_recording_allowed())

    def test_inactivity_never_sleeps_when_voice_busy(self):
        due = self.api.kim_menu_gate_test_sleep_due
        self.assertFalse(due(100000, 0, 30, True, False, False))
        self.assertFalse(due(100000, 0, 30, False, True, False))
        self.assertFalse(due(100000, 0, 30, False, False, True))
        self.assertFalse(due(100000, 0, 0, False, False, False))
        self.assertFalse(due(29999, 0, 30, False, False, False))
        self.assertTrue(due(30000, 0, 30, False, False, False))
        self.api.kim_menu_gate_test_open(True)
        self.assertFalse(due(100000, 0, 30, False, False, False))
    def test_sleep_timer_handles_tick_wrap_and_gate(self):
        due = self.api.kim_menu_gate_test_sleep_due
        self.assertTrue(due(1500, 0xFFFF0000, 30, False, False, False))
        self.api.kim_menu_gate_test_sleep(True)
        self.assertFalse(due(1500, 0xFFFF0000, 30, False, False, False))

    def test_close_while_boot_pressed_does_not_arm_touch(self):
        self.api.kim_menu_gate_test_open(True)
        self.api.kim_menu_gate_test_open(False)
        self.api.kim_menu_gate_test_step(10, False, True)
        self.assertFalse(self.api.kim_menu_recording_allowed())
        self.api.kim_menu_gate_test_step(20, True, False)
        self.assertFalse(self.api.kim_menu_recording_allowed())
        self.api.kim_menu_gate_test_step(30, False, False)
        self.assertTrue(self.api.kim_menu_recording_allowed())


if __name__ == "__main__":
    unittest.main()
