"""Exercise real settings C with pthread-backed FreeRTOS mutex and NVS mock."""
import subprocess
import tempfile
from pathlib import Path

MAIN = Path(__file__).resolve().parents[1] / 'firmware/main'


def test_concurrent_settings_network_transactions():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / 'freertos').mkdir()
        (d / 'esp_err.h').write_text('''typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_INVALID_STATE 259
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_NVS_NOT_FOUND 4354
#define ESP_ERR_NVS_INVALID_LENGTH 4356
''')
        (d / 'nvs.h').write_text('''#include <stddef.h>
typedef int nvs_handle_t;
#define NVS_READWRITE 1
int nvs_open(const char*,int,nvs_handle_t*);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
''')
        (d / 'freertos/FreeRTOS.h').write_text('#define portMAX_DELAY 0xffffffffu\n')
        (d / 'freertos/semphr.h').write_text('''#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { SemaphoreHandle_t p=malloc(sizeof(*p)); if(p) pthread_mutex_init(p,0); return p; }
static inline int xSemaphoreTake(SemaphoreHandle_t p,unsigned t) { (void)t; return pthread_mutex_lock(p)==0; }
static inline int xSemaphoreGive(SemaphoreHandle_t p) { return pthread_mutex_unlock(p)==0; }
''')
        (d / 'harness.c').write_text('''#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include "kim_settings.h"
#include "nvs.h"
static unsigned char prefs[1024], nets[1024];
static size_t ps, ns;
static int fail_commit;
int nvs_open(const char *name,int mode,nvs_handle_t *h) { (void)name;(void)mode;*h=1;return 0; }
int nvs_get_blob(nvs_handle_t h,const char *key,void *v,size_t *size) {
    (void)h; size_t n=!strcmp(key,"prefs")?ps:ns;
    if(!n) return ESP_ERR_NVS_NOT_FOUND;
    if(*size<n) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(v,!strcmp(key,"prefs")?prefs:nets,n); *size=n; return 0;
}
int nvs_set_blob(nvs_handle_t h,const char *key,const void *v,size_t size) {
    (void)h; assert(size<=1024);
    if(!strcmp(key,"prefs")){memcpy(prefs,v,size);ps=size;}
    else {memcpy(nets,v,size);ns=size;}
    sched_yield(); return 0;
}
int nvs_commit(nvs_handle_t h){(void)h;sched_yield();return fail_commit?ESP_ERR_INVALID_STATE:0;}
void nvs_close(nvs_handle_t h){(void)h;}
typedef struct {int role;} arg_t;
static void *writer(void *v) {
    int role=((arg_t*)v)->role;
    for(int i=0;i<500;i++) {
        if(role==0) assert(kim_settings_set_volume(10+i%70)==0);
        if(role==1) assert(kim_settings_set_brightness(10+i%90)==0);
        if(role==2) assert(kim_settings_set_sleep_s(30+i%1700)==0);
        if(role==3) assert(kim_settings_network_save("alpha","abcdefgh")==0);
        if(role==4) assert(kim_settings_network_save("beta","ijklmnop")==0);
        if(role==5) { kim_settings_network_t n; int count=kim_settings_network_count();
            assert(count>=0 && count<=2);
            for(int j=0;j<count;j++) if(kim_settings_network_get(j,&n)) {
                assert(n.ssid[0]=='a'||n.ssid[0]=='b');
                assert(!strcmp(n.password,"abcdefgh")||!strcmp(n.password,"ijklmnop"));
            }
        }
    }
    return NULL;
}
int main(void) {
    assert(kim_settings_init()==ESP_OK);
    pthread_t threads[6]; arg_t args[6];
    for(int i=0;i<6;i++){ args[i].role=i;assert(!pthread_create(&threads[i],0,writer,&args[i])); }
    for(int i=0;i<6;i++) pthread_join(threads[i],0);
    int v=kim_settings_volume(), b=kim_settings_brightness(), s=kim_settings_sleep_s();
    assert(kim_settings_network_count()==2);
    assert(kim_settings_init()==ESP_OK);
    assert(kim_settings_volume()==v && kim_settings_brightness()==b && kim_settings_sleep_s()==s);
    assert(kim_settings_network_count()==2);
    fail_commit=1; assert(kim_settings_set_volume(80)!=ESP_OK); assert(kim_settings_volume()==v);
    assert(kim_settings_network_forget("alpha")!=ESP_OK); assert(kim_settings_network_count()==2);
    puts("settings: six concurrent real C paths, persisted scalar RMW and network snapshots; failure rollback");
    return 0;
}
''')
        exe = d / 'probe'
        subprocess.run(['cc', '-std=c11', '-pthread', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(d), '-I', str(MAIN), str(d / 'harness.c'),
                        str(MAIN / 'kim_settings.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
