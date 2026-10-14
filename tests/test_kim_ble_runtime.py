"""Compile and execute the actual BLE GAP/GATT/worker C under host shims."""
import subprocess
import tempfile
import resource
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware/main'
HOST = ROOT / 'tests/ble_host'
JSON = ROOT / 'firmware/managed_components/espressif__cjson/cJSON'


def _compile_and_run(source, *, expect_failure=False, lifecycle=False, startup=False, fault=None, sanitize=False):
    marker = '#define WINDOW_MS 180000U'
    assert source.count(marker) == 1
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / 'ble_body.c').write_text(source[source.index(marker):])
        exe = d / 'run'
        subprocess.run(['cc', '-std=c11', '-pthread', '-w', *(['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitize else []), *(['-DFAULT_TEST'] if fault else ['-DLIFECYCLE_TEST'] if lifecycle else ['-DSTARTUP_TEST'] if startup else []), '-I', str(d), '-I', str(HOST),
                        '-I', str(MAIN), '-I', str(JSON), str(HOST / 'harness.c'),
                        str(MAIN / 'kim_ble_protocol.c'), str(JSON / 'cJSON.c'),
                        '-lm', '-o', str(exe)], check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(exe), *([str(fault)] if fault else [])], capture_output=True, timeout=15, preexec_fn=no_core)
        if expect_failure:
            assert result.returncode != 0, 'original wipe bug escaped the runtime harness'
        else:
            assert result.returncode == 0, result.stderr.decode(errors='replace')


def test_ble_runtime_state_and_terminal_persistence():
    source = (MAIN / 'kim_ble.c').read_text()
    _compile_and_run(source)


def test_boot_and_health_idle_do_not_initialize_nimble_until_physical_setup():
    _compile_and_run((MAIN / 'kim_ble.c').read_text(), startup=True)


def test_original_early_wipe_fails_same_runtime_harness():
    source = (MAIN / 'kim_ble.c').read_text()
    original = 'memset(req.password,0,sizeof req.password);'
    assert source.count(original) == 1
    _compile_and_run(source.replace(original, 'kim_ble_wipe(&req);'), expect_failure=True)


def test_lifecycle_cleanup_prevents_cross_session_admission():
    _compile_and_run((MAIN / 'kim_ble.c').read_text(), lifecycle=True)


def test_unconditional_rejected_disconnect_fails_same_runtime_harness():
    source = (MAIN / 'kim_ble.c').read_text()
    guard = 'if(ours) {\n            connection=BLE_HS_CONN_HANDLE_NONE;'
    assert source.count(guard) == 1
    _compile_and_run(source.replace(guard, 'if(true) {\n            connection=BLE_HS_CONN_HANDLE_NONE;'),
                     expect_failure=True)


def test_actual_c_resource_faults_and_fail_closed_recovery():
    source = (MAIN / 'kim_ble.c').read_text()
    for mode in range(1, 15):
        _compile_and_run(source, fault=mode)


def test_setup_close_and_expiry_during_tls_admission_wait():
    source = (MAIN / 'kim_ble.c').read_text()
    for mode in (15, 16):
        _compile_and_run(source, fault=mode)


def test_setup_close_and_expiry_at_resource_acquisition_boundary():
    source = (MAIN / 'kim_ble.c').read_text()
    for mode in (17, 18):
        _compile_and_run(source, fault=mode)


def test_held_real_tls_probe_past_setup_wait_fails_without_restart():
    _compile_and_run((MAIN / 'kim_ble.c').read_text(), fault=19)


def test_setup_admission_cancellation_asan_ubsan():
    source = (MAIN / 'kim_ble.c').read_text()
    for mode in (15, 16, 17, 18, 19):
        _compile_and_run(source, fault=mode, sanitize=True)


def test_actual_c_lifecycle_asan_ubsan():
    _compile_and_run((MAIN / 'kim_ble.c').read_text(), lifecycle=True, sanitize=True)


def test_actual_runtime_atomic_tls_ble_admission_asan_ubsan():
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'resource'
        subprocess.run(['cc', '-std=c11', '-pthread', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I', str(MAIN),
                        str(HOST / 'resource_runtime.c'), str(MAIN / 'kim_runtime.c'),
                        '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, timeout=15)
        assert result.returncode == 0, result.stderr.decode(errors='replace')


def test_partial_init_old_release_path_fails_runtime_regression():
    source = (MAIN / 'kim_ble.c').read_text()
    guard = 'if(err!=ESP_OK) fail_closed("NimBLE partial init failed");'
    assert source.count(guard) == 1
    _compile_and_run(source.replace(guard, 'if(err!=ESP_OK) goto failed_host;'),
                     fault=3, expect_failure=True)


def test_missing_resource_admission_fails_interleaving_regression():
    source = (MAIN / 'kim_ble.c').read_text()
    assert source.count('if(kim_ble_resource_begin()) break;') == 1
    _compile_and_run(source.replace('if(kim_ble_resource_begin()) break;', 'if(true) break;')
                     .replace('kim_ble_resource_end();', '(void)0;'),
                     fault=13, expect_failure=True)
