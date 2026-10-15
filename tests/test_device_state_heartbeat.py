"""Exercise the production privacy scheduler/JSON and transport contract."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware/main'


def test_privacy_heartbeat_actual_c():
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp) / 'privacy'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(MAIN), str(MAIN / 'kim_device_state.c'),
                        str(ROOT / 'tests/device_state_harness.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_transport_actual_c_posts_exact_json_without_secret_logging():
    source = (MAIN / 'voice.c').read_text()
    section = source.split('static kim_device_state_t device_state;', 1)[1].split('/* A separate endpoint', 1)[0]
    shim = r'''
#include "kim_device_state.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define ESP_OK 0
#define HTTP_METHOD_POST 1
#define BSP_BUTTON_MUTE_IO 1
#define KIM_SERVER_URL "https://unit.invalid/v1/turn"
#define KIM_DEVICE_TOKEN "synthetic-not-a-real-secret-123456789012"
#define KIM_SERVER_CA_PEM "synthetic CA"
typedef int esp_err_t;
typedef struct {const char *url, *cert_pem; void *crt_bundle_attach; int timeout_ms, buffer_size; bool disable_auto_redirect;} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static void *esp_crt_bundle_attach;
static atomic_bool kim_is_online=true;
static bool mock_muted, mock_menu;
static int64_t clock_us;
static int calls, probe_start, probe_end, status=204;
static char sent_body[96], sent_url[256];
static int64_t esp_timer_get_time(void) {return clock_us;}
static int gpio_get_level(int pin) {(void)pin; return !mock_muted;}
static bool kim_menu_open(void) {return mock_menu;}
static bool kim_tls_probe_begin(void) {++probe_start; return true;}
static void kim_tls_probe_end(void) {++probe_end;}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    assert(!strcmp(cfg->cert_pem, KIM_SERVER_CA_PEM));
    assert(cfg->timeout_ms == 300 && cfg->buffer_size == 256 && cfg->disable_auto_redirect);
    snprintf(sent_url, sizeof(sent_url), "%s", cfg->url);
    return (void *)1;
}
static void esp_http_client_set_method(esp_http_client_handle_t c, int method) {(void)c; assert(method==HTTP_METHOD_POST);}
static int esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value) {
    (void)c;
    if (!strcmp(key, "Authorization")) assert(!strcmp(value, "Bearer " KIM_DEVICE_TOKEN));
    else {assert(!strcmp(key, "Content-Type")); assert(!strcmp(value, "application/json"));}
    return ESP_OK;
}
static int esp_http_client_set_post_field(esp_http_client_handle_t c, const char *body, int len) {
    (void)c; assert(len>0 && len<(int)sizeof(sent_body));
    memcpy(sent_body,body,len); sent_body[len]=0;
    return ESP_OK;
}
static int esp_http_client_perform(esp_http_client_handle_t c) {(void)c; ++calls; return ESP_OK;}
static int esp_http_client_get_status_code(esp_http_client_handle_t c) {(void)c; return status;}
static void esp_http_client_cleanup(esp_http_client_handle_t c) {(void)c;}
static kim_device_state_t device_state;
''' + section + r'''
int main(void) {
    device_state_report();
    assert(calls==1 && probe_start==1 && probe_end==1);
    assert(!strcmp(sent_url,"https://unit.invalid/v1/device-state"));
    assert(!strcmp(sent_body,"{\"version\":1,\"epoch\":1,\"muted\":false,\"menu_open\":false}"));
    assert(!strstr(sent_body,"synthetic-not-a-real-secret"));
    clock_us=4999000; device_state_report(); assert(calls==1);
    clock_us=5000000; device_state_report(); assert(calls==2);
    mock_muted=true; clock_us=5001000; device_state_report();
    assert(calls==3 && !strcmp(sent_body,"{\"version\":1,\"epoch\":2,\"muted\":true,\"menu_open\":false}"));
    mock_menu=true; clock_us=5002000; device_state_report();
    assert(calls==4 && !strcmp(sent_body,"{\"version\":1,\"epoch\":3,\"muted\":true,\"menu_open\":true}"));
    status=500; clock_us=10002000; device_state_report(); assert(calls==5);
    clock_us=10003000; device_state_report(); assert(calls==5);
    clock_us=10253000; device_state_report(); assert(calls==6);
    assert(mock_muted && mock_menu);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        harness = Path(temp) / 'transport.c'
        binary = Path(temp) / 'transport'
        harness.write_text(shim)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(MAIN),
                        str(harness), str(MAIN / 'kim_device_state.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_transport_is_low_priority_authenticated_and_never_logs_payload():
    source = (MAIN / 'voice.c').read_text()
    worker = source.split('static void character_task(void *arg) {\n', 1)[1].split('\n}\n', 1)[0]
    assert worker.index('device_state_report(') < worker.index('character_probe();')
    report = source.split('static void device_state_report(', 1)[1].split('\n}\n', 1)[0]
    assert 'kim_device_state_format(' in report
    assert 'HTTP_METHOD_POST' in report
    assert '"Authorization", auth' in report
    assert '"Content-Type", "application/json"' in report
    assert 'KIM_SERVER_CA_PEM[0] ? KIM_SERVER_CA_PEM : NULL' in report
    assert 'esp_crt_bundle_attach' in report
    assert 'esp_http_client_get_status_code(client) == 204' in report
    assert 'ESP_LOG' not in report
    assert 'kim_tls_probe_begin()' in report
    assert 'kim_tls_probe_end()' in report
    assert 'kim_device_state_epoch(' in source.split('static void character_probe(', 1)[1].split('\n}\n', 1)[0]
