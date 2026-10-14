import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"


def compile_and_run(code, *sources, flags=()):
    with tempfile.TemporaryDirectory(dir=Path.home() / ".hermes/cache/scratch") as tmp:
        source = Path(tmp) / "probe.c"
        binary = Path(tmp) / "probe"
        source.write_text(code)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MAIN),
                        *flags, str(source), *(str(MAIN / src) for src in sources),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_maintenance_gate_spans_voice_and_ota():
    compile_and_run('''#include <assert.h>
#include "kim_runtime.h"
int main(void) {
    assert(kim_runtime_voice_begin());
    assert(kim_runtime_voice_busy());
    assert(!kim_maintenance_acquire());
    assert(!kim_runtime_voice_begin());
    kim_runtime_voice_end();
    assert(!kim_runtime_voice_busy());
    assert(kim_maintenance_acquire());
    assert(!kim_runtime_voice_begin());
    kim_maintenance_release();
    assert(kim_runtime_voice_begin());
    kim_runtime_voice_end();
    return 0;
}''', "kim_runtime.c")


def test_menu_cancel_interrupts_reply_after_release():
    compile_and_run('''#include <assert.h>
#include "touch_ptt.h"
int main(void) {
    kim_touch_ptt_t p = {0};
    kim_touch_press(&p);
    unsigned turn = atomic_load(&p.epoch);
    kim_touch_release(&p);
    assert(kim_touch_submit(&p, turn, 3200, false, true));
    assert(!kim_touch_cancelled(&p, turn));
    kim_ptt_cancel(&p); /* Menu opens while network/speaker is still active. */
    assert(kim_touch_cancelled(&p, turn));
    kim_touch_press(&p);
    assert(kim_touch_recording(&p, turn + 1));
    return 0;
}''')


def test_menu_and_wake_requires_full_release_of_both_inputs():
    compile_and_run('''#include <assert.h>
#include <stdint.h>
#include "kim_menu.h"
void kim_menu_gate_test_reset(void);
void kim_menu_gate_test_open(_Bool);
void kim_menu_gate_test_sleep(_Bool);
void kim_menu_gate_test_step(uint32_t, _Bool, _Bool);
_Bool kim_menu_gate_test_sleep_due(uint32_t, uint32_t, int, _Bool, _Bool, _Bool);
int main(void) {
    kim_menu_gate_test_reset();
    assert(kim_menu_recording_allowed());
    kim_menu_gate_test_open(1);
    assert(!kim_menu_recording_allowed());
    kim_menu_gate_test_open(0);
    kim_menu_gate_test_step(1, 1, 1);
    assert(!kim_menu_recording_allowed());
    kim_menu_gate_test_step(2, 0, 1);
    assert(!kim_menu_recording_allowed());
    kim_menu_gate_test_step(3, 0, 0);
    assert(kim_menu_recording_allowed());
    kim_menu_gate_test_sleep(1);
    kim_menu_gate_test_sleep(0);
    kim_menu_gate_test_step(4, 0, 1);
    assert(!kim_menu_recording_allowed());
    kim_menu_gate_test_step(5, 0, 0);
    assert(kim_menu_recording_allowed());
    assert(!kim_menu_gate_test_sleep_due(300001, 0, 300, 1, 0, 0));
    assert(kim_menu_gate_test_sleep_due(300001, 0, 300, 0, 0, 0));
    return 0;
}''', "kim_menu.c", flags=("-DKIM_MENU_GATE_TEST",))


def test_partition_bounds_and_bootstrap_order():
    import csv
    rows = list(csv.reader((ROOT / "firmware/partitions.csv").read_text().splitlines()))
    partitions = {r[0]: (int(r[3], 0), int(r[4], 0)) for r in rows if r and not r[0].startswith("#")}
    assert partitions["nvs"] == (0x9000, 0x6000)
    assert partitions["phy_init"] == (0xF000, 0x1000)
    assert partitions["factory"] == (0x10000, 0x400000)
    assert partitions["otadata"] == (0x410000, 0x2000)
    assert partitions["ota_0"] == (0x420000, 0x400000)
    assert partitions["ota_1"] == (0x820000, 0x400000)
    spans = sorted((offset, offset + size) for offset, size in partitions.values())
    assert all(end <= next_start for (_, end), (next_start, _) in zip(spans, spans[1:]))
    assert spans[-1][1] <= 0x1000000
    main = (MAIN / "main.c").read_text()
    assert main.index("nvs_flash_init()") < main.index("kim_settings_init()") < main.index("kim_menu_init(")
    assert main.index("kim_settings_init()") < main.index("kim_wifi_start(")
    assert "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in (ROOT / "firmware/sdkconfig").read_text()


def test_voice_gate_and_backend_health_are_reached_before_capture():
    voice = (MAIN / "voice.c").read_text()
    main = (MAIN / "main.c").read_text()
    assert voice.index("esp_netif_sntp_sync_wait") < voice.index("kim_backend_healthy = health_probe()")
    assert voice.index("kim_runtime_voice_begin()") < voice.index("esp_codec_dev_open(mic, &fs)")
    # The LAST voice_end in the task loop closes the PTT turn after its
    # send_response(size); an earlier hands-free block owns its own end().
    assert voice.rindex("kim_runtime_voice_end()") > voice.index("send_response(size)")
    assert main.index("kim_menu_tick(") < main.index("kim_boot_sample(")
    assert "kim_ptt_cancel(&kim_touch_ptt)" in main
    # Health alone must not validate a trial OTA image. Confirmation routes
    # through kim_ota_trial_poll (capture/reply/menu evidence), invoked from
    # the main loop; a static kim_ota_confirm_healthy() call would bypass it.
    assert "kim_ota_trial_poll(" in main
    assert "kim_ota_confirm_healthy()" not in main
