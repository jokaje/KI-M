"""Compile the production (non KIM_VOICE_HOST_TEST) task/FIFO and HTTP path."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_worker_http_fifo_path():
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp) / "worker-test"
        subprocess.run([
            "cc", "-D_DEFAULT_SOURCE", "-std=c11", "-Wall", "-Wextra", "-pthread",
            "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-const-variable",
            "-I", str(ROOT / "firmware/main"),
            str(ROOT / "tests/voice_host/worker_test.c"),
            *[str(ROOT / f"firmware/main/{name}.c") for name in
              ("audio_jitter", "pcm_stream", "playback_clock")],
            "-o", str(binary),
        ], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                check=True, timeout=30)
        assert "worker-path PASS" in result.stdout
