import ctypes
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'firmware/main'


class AP(ctypes.Structure):
    _fields_ = [('ssid', ctypes.c_char * 33), ('rssi', ctypes.c_int), ('saved', ctypes.c_bool), ('secure', ctypes.c_bool)]


class WifiTest(unittest.TestCase):
    def test_visible_preference_security_failover_and_async_actions(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG 258\n#define ESP_ERR_INVALID_STATE 259\n#define ESP_ERR_NO_MEM 257\n#define ESP_ERR_NVS_NOT_FOUND 4354\n#define ESP_ERR_NVS_INVALID_LENGTH 4356\n#define ESP_ERR_TIMEOUT 263\n')
            (p / 'esp_wifi.h').write_text('''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#define WIFI_EVENT ((void*)1)
#define WIFI_EVENT_SCAN_DONE 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_STA_START 3
#define IP_EVENT ((void*)2)
#define IP_EVENT_STA_GOT_IP 4
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 0
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WEP 1
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_AUTH_WPA3_PSK 6
#define WIFI_AUTH_WPA2_WPA3_PSK 7
#define WIFI_SCAN_TYPE_ACTIVE 0
#define WIFI_ALL_CHANNEL_SCAN 0
#define WIFI_CONNECT_AP_BY_SIGNAL 0
#define WIFI_PS_MIN_MODEM 1
typedef struct {uint8_t ssid[33]; int8_t rssi; int authmode;} wifi_ap_record_t;
typedef struct {uint8_t ssid[32],password[64]; struct {int authmode;} threshold; struct {bool capable,required;} pmf_cfg;} wifi_sta_config_t;
typedef struct {wifi_sta_config_t sta;} wifi_config_t;
typedef struct {int dummy;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct {bool show_hidden;} wifi_scan_config_t;
int esp_wifi_init(const wifi_init_config_t*); int esp_wifi_set_storage(int); int esp_wifi_set_mode(int); int esp_wifi_start(void); int esp_wifi_scan_start(const wifi_scan_config_t*,bool); int esp_wifi_scan_get_ap_records(uint16_t*,wifi_ap_record_t*); int esp_wifi_clear_ap_list(void); int esp_wifi_set_config(int,wifi_config_t*); int esp_wifi_connect(void); int esp_wifi_disconnect(void);
''')
            (p / 'esp_netif.h').write_text('int esp_netif_init(void); void *esp_netif_create_default_wifi_sta(void);\n')
            (p / 'esp_event.h').write_text('''#include <stdint.h>
typedef void *esp_event_base_t;
typedef void (*esp_event_handler_t)(void*,esp_event_base_t,int32_t,void*);
int esp_event_loop_create_default(void); int esp_event_handler_register(esp_event_base_t,int32_t,esp_event_handler_t,void*);
#define ESP_EVENT_ANY_ID -1
''')
            (p / 'esp_log.h').write_text('#define ESP_LOGW(...) ((void)0)\n')
            (p / 'freertos').mkdir()
            (p / 'freertos/FreeRTOS.h').write_text('''#include <stdint.h>
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(n) (n)
typedef unsigned TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
''')
            (p / 'freertos/queue.h').write_text('''#include "freertos/FreeRTOS.h"
typedef void* QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned,unsigned); int xQueueSend(QueueHandle_t,const void*,TickType_t); int xQueueReceive(QueueHandle_t,void*,TickType_t);
''')
            (p / 'freertos/task.h').write_text('''#include "freertos/FreeRTOS.h"
typedef void* TaskHandle_t;
int xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*); void vTaskDelay(TickType_t); TickType_t xTaskGetTickCount(void);
''')
            (p / 'mock.c').write_text('''#include <string.h>
#include <pthread.h>
#include <time.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kim_settings.h"
static void (*worker)(void*); static void *worker_arg; static esp_event_handler_t handler;
static pthread_t thread; static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct {int kind; unsigned id; char ssid[33], password[64];} queue[32]; static int qhead,qtail;
static wifi_ap_record_t aps[8]; static int ap_count; static char selected[33]; static int attempts, scans;
static int saves, forgets, persist_error;
int kim_settings_init(void){return 0;} int kim_settings_network_count(void){return 2;}
bool kim_settings_network_get(int i,kim_settings_network_t *out){memset(out,0,sizeof(*out));if(i==0){strcpy(out->ssid,"preferred");strcpy(out->password,"12345678");}else if(i==1){strcpy(out->ssid,"backup");strcpy(out->password,"abcdefgh");}else return false;return true;}
int kim_settings_network_save(const char *s,const char *p){saves++;return persist_error;}
int kim_settings_network_forget(const char *s){forgets++;return persist_error;}
void mock_persist_error(int error){persist_error=error;}
int esp_netif_init(void){return 0;} void *esp_netif_create_default_wifi_sta(void){return (void*)1;}
int esp_event_loop_create_default(void){return 0;}
int esp_event_handler_register(esp_event_base_t b,int32_t id,esp_event_handler_t h,void *p){handler=h;return 0;}
int esp_wifi_init(const wifi_init_config_t *c){return 0;} int esp_wifi_set_storage(int i){return 0;} int esp_wifi_set_mode(int i){return 0;} int esp_wifi_start(void){return 0;}
int esp_wifi_scan_start(const wifi_scan_config_t *c,bool b){scans++;return 0;}
int esp_wifi_scan_get_ap_records(uint16_t *n,wifi_ap_record_t *a){int count=*n<ap_count?*n:ap_count;memcpy(a,aps,count*sizeof(*a));*n=count;return 0;}
int esp_wifi_clear_ap_list(void){return 0;}
int esp_wifi_set_config(int i,wifi_config_t *c){memcpy(selected,c->sta.ssid,32);selected[32]=0;return 0;}
int esp_wifi_connect(void){attempts++;return 0;} int esp_wifi_disconnect(void){return 0;}
QueueHandle_t xQueueCreate(unsigned n,unsigned s){return (void*)1;}
int xQueueSend(QueueHandle_t q,const void *p,TickType_t t){pthread_mutex_lock(&mutex);int ok=qtail<32;if(ok)memcpy(&queue[qtail++],p,sizeof(queue[0]));pthread_mutex_unlock(&mutex);return ok;}
int xQueueReceive(QueueHandle_t q,void *p,TickType_t t){struct timespec ts={0,1000000};nanosleep(&ts,0);pthread_mutex_lock(&mutex);int ok=qhead<qtail;if(ok)memcpy(p,&queue[qhead++],sizeof(queue[0]));pthread_mutex_unlock(&mutex);return ok;}
static void *run(void *p){worker(worker_arg);return 0;}
int xTaskCreate(void (*fn)(void*),const char *n,unsigned sz,void *arg,unsigned pri,TaskHandle_t *h){worker=fn;worker_arg=arg;return pthread_create(&thread,0,run,0)==0;}
void mock_stop(void){pthread_cancel(thread);pthread_join(thread,0);}
void vTaskDelay(TickType_t t){} TickType_t xTaskGetTickCount(void){static unsigned tick;return tick+=1000;}
void mock_ap(const char *name,int rssi,int auth){wifi_ap_record_t *a=&aps[ap_count++];strcpy((char*)a->ssid,name);a->rssi=rssi;a->authmode=auth;}
void mock_event(int id){handler(0,id==4?IP_EVENT:WIFI_EVENT,id,0);}
const char *mock_selected(void){return selected;} int mock_attempts(void){return attempts;} int mock_scans(void){return scans;}
int mock_saves(void){return saves;} int mock_forgets(void){return forgets;}
''')
            lib = p / 'wifi.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-pthread', '-std=c11', '-Wall', '-Werror', '-I', str(p), '-I', str(ROOT), str(ROOT / 'kim_wifi.c'), str(p / 'mock.c'), '-o', str(lib)], check=True)
            c = ctypes.CDLL(str(lib))
            c.kim_wifi_start.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            c.kim_wifi_scan_results.argtypes = [ctypes.POINTER(AP), ctypes.c_int]
            c.kim_wifi_save.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            c.mock_ap.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
            c.mock_selected.restype = ctypes.c_char_p
            self.assertEqual(c.kim_wifi_start(b'legacy', b'12345678'), 0)
            self.assertEqual(c.mock_scans(), 0)  # scan occurs in worker, not caller
            c.mock_ap(b'backup', -30, 3)
            c.mock_ap(b'preferred', -80, 6)
            c.mock_ap(b'open', -10, 0)
            c.mock_ap(b'preferred', -20, 0)  # insecure duplicate must not hide saved WPA3
            self.assertEqual(c.kim_wifi_scan(), 0)
            def until(predicate):
                for _ in range(100):
                    if predicate(): return
                    time.sleep(.005)
                self.fail('worker did not reach expected radio state')
            try:
                until(lambda: c.mock_scans() > 0)
                c.mock_event(1)  # scan done
                until(lambda: c.mock_attempts() == 1)
                self.assertEqual(c.mock_selected(), b'preferred')  # preference beats RSSI
                aps = (AP * 8)()
                until(lambda: c.kim_wifi_scan_results(aps, 8) == 3)
                self.assertTrue(aps[0].saved)
                self.assertFalse(aps[2].secure)
                c.mock_event(2)  # failed association
                until(lambda: c.mock_scans() > 1)
                c.mock_event(1)
                until(lambda: c.mock_attempts() == 2)
                self.assertEqual(c.mock_selected(), b'preferred')
                c.mock_event(2)
                until(lambda: c.mock_scans() > 2)
                c.mock_event(1)
                until(lambda: c.mock_attempts() == 3)
                self.assertEqual(c.mock_selected(), b'backup')
                self.assertEqual(c.kim_wifi_scan(), 0)  # queued during association
                c.mock_event(4)
                until(lambda: c.kim_wifi_connected())
                until(lambda: c.mock_scans() > 3)
                self.assertTrue(c.kim_wifi_scan_busy())
                c.mock_event(1)
                until(lambda: not c.kim_wifi_scan_busy())
                c.kim_wifi_forget.argtypes = [ctypes.c_char_p]
                c.mock_persist_error.argtypes = [ctypes.c_int]
                c.mock_persist_error(4354)
                op = ctypes.c_uint(0)
                c.kim_wifi_save_tracked.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint)]
                c.kim_wifi_forget_tracked.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint)]
                c.kim_wifi_operation_result.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int)]
                self.assertEqual(c.kim_wifi_save_tracked(b'fail', b'12345678', ctypes.byref(op)), 0)
                status = ctypes.c_int()
                until(lambda: c.kim_wifi_operation_result(op.value, ctypes.byref(status)) == 1)
                self.assertEqual(status.value, 4354)
                self.assertEqual(c.kim_wifi_forget_tracked(b'backup', ctypes.byref(op)), 0)
                until(lambda: c.kim_wifi_operation_result(op.value, ctypes.byref(status)) == 1)
                self.assertEqual(status.value, 4354)
                c.mock_persist_error(0)
                self.assertNotEqual(c.kim_wifi_save(b'bad', b'short'), 0)
                self.assertEqual(c.kim_wifi_save(b'new', b'12345678'), 0)
                until(lambda: c.mock_saves() == 2)
                c.kim_wifi_forget.argtypes = [ctypes.c_char_p]
                self.assertEqual(c.kim_wifi_forget(b'backup'), 0)
                until(lambda: c.mock_forgets() == 2)
            finally:
                c.mock_stop()


if __name__ == '__main__':
    unittest.main()
