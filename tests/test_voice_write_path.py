"""Production voice.c stream and WAV codec-write integration under host shims."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_actual_voice_write_paths():
    with tempfile.TemporaryDirectory() as tmp:
        binary = pathlib.Path(tmp) / "voice_write_test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-const-variable",
                        "-I", str(ROOT / "firmware/main"),
                        str(ROOT / "tests/voice_host/voice_write_test.c"),
                        str(ROOT / "firmware/main/playback_clock.c"),
                        "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
        assert "PASS" in result.stdout
