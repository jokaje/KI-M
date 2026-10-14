#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "stubs.h"
#include "ble_body.c" /* actual firmware worker, GATT, GAP and manager */
static pthread_mutex_t mux=PTHREAD_MUTEX_INITIALIZER;
static struct test_queue {kim_ble_request_t request; int full;} queue;
static uint32_t clock_ms=100;
static int notify_count, operation_ready=1, wifi_result_error;
static int nimble_inits, nimble_deinits, host_starts, restarts, gate_held;
static int fail_queue, fail_init, fail_io, fail_host, fail_manager, fail_stop, fail_deinit, wedge_stop;
static int manager_paused, manager_go=1, host_stopped, real_tasks;
static int resource_owner, init_paused, init_go=1, deinit_paused, deinit_go=1;
static int setup_race, resource_attempts;
static int security_key_size=16, security_bonded, disconnect_on_terminate;
static uint16_t terminated;
static pthread_t tasks[64]; static int task_count;
void portENTER_CRITICAL(portMUX_TYPE *p){(void)p; pthread_mutex_lock(&mux);}
void portEXIT_CRITICAL(portMUX_TYPE *p){(void)p; pthread_mutex_unlock(&mux);}
size_t heap_caps_get_free_size(int caps){(void)caps;return 100000;}
size_t heap_caps_get_largest_free_block(int caps){(void)caps;return 50000;}
uint32_t xTaskGetTickCount(void){return __atomic_load_n(&clock_ms,__ATOMIC_RELAXED);}
void vTaskDelay(unsigned ms){struct timespec t={.tv_sec=0,.tv_nsec=1000000};nanosleep(&t,NULL);__atomic_add_fetch(&clock_ms,ms?ms:1,__ATOMIC_RELAXED);}
void vTaskDelete(void *task){(void)task;pthread_exit(NULL);}
QueueHandle_t xQueueCreate(unsigned n,unsigned size){(void)n;assert(size==sizeof(kim_ble_request_t));if(fail_queue)return NULL;assert(!queue.full);return &queue;}
void vQueueDelete(QueueHandle_t q){assert(q==&queue && !q->full);}
int xQueueSend(QueueHandle_t q,const void *v,int t){(void)t; assert(q==&queue);pthread_mutex_lock(&mux);int ok=!q->full;if(ok){memcpy(&q->request,v,sizeof q->request);q->full=1;}pthread_mutex_unlock(&mux);return ok;}
int xQueueReceive(QueueHandle_t q,void *v,unsigned t){(void)t;assert(q==&queue);pthread_mutex_lock(&mux);int ok=q->full;if(ok){memcpy(v,&q->request,sizeof q->request);memset(&q->request,0,sizeof q->request);q->full=0;}pthread_mutex_unlock(&mux);return ok;}
struct task_start {void (*fn)(void *); void *arg;};
static void *task_entry(void *p){struct task_start s=*(struct task_start *)p;free(p);
    if(s.fn==manager_task){manager_paused=1;while(!manager_go)sched_yield();}
    if(s.fn==host_task){host_starts++;sync_cb();}
    s.fn(s.arg);return NULL;
}
int xTaskCreate(void (*fn)(void*),const char *name,int stack,void *arg,int priority,void *task){
    (void)stack;(void)priority;(void)task;
    if((!strcmp(name,"kim-ble-manager") && fail_manager) || (!strcmp(name,"kim-ble-io") && fail_io) || (!strcmp(name,"kim-ble-stop") && fail_stop))return 0;
    struct task_start *start=malloc(sizeof *start);assert(start);*start=(struct task_start){fn,arg};
    assert(task_count<64);int rc=pthread_create(&tasks[task_count++],NULL,task_entry,start);assert(!rc);return pdPASS;
}
int xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,int stack,void *arg,int priority,void *task,int core){(void)core;if(fail_host)return 0;return xTaskCreate(fn,name,stack,arg,priority,task);}
int nimble_port_init(void){nimble_inits++;init_paused=1;while(!init_go)sched_yield();return fail_init ? ESP_FAIL : ESP_OK;}
int nimble_port_deinit(void){nimble_deinits++;deinit_paused=1;while(!deinit_go)sched_yield();return fail_deinit?ESP_FAIL:ESP_OK;}
void nimble_port_run(void){while(!host_stopped)vTaskDelay(1);}
int nimble_port_stop(void){if(wedge_stop){for(;;) vTaskDelay(100);}host_stopped=1;return fail_stop==2?ESP_FAIL:ESP_OK;}
void esp_restart(void){restarts++;pthread_exit(NULL);}
bool kim_maintenance_acquire(void){return !__atomic_exchange_n(&gate_held,1,__ATOMIC_SEQ_CST);}
void kim_maintenance_release(void){assert(__atomic_exchange_n(&gate_held,0,__ATOMIC_SEQ_CST)==1);}
bool kim_tls_probe_begin(void){int zero=0;return __atomic_compare_exchange_n(&resource_owner,&zero,1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);}
void kim_tls_probe_end(void){assert(__atomic_exchange_n(&resource_owner,0,__ATOMIC_SEQ_CST)==1);}
bool kim_ble_resource_begin(void){int zero=0;
    __atomic_add_fetch(&resource_attempts,1,__ATOMIC_SEQ_CST);
    bool acquired=__atomic_compare_exchange_n(&resource_owner,&zero,2,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    /* Execute navigation/expiry at the exact gate acquisition boundary. */
    if(acquired && setup_race){int race=setup_race;setup_race=0;
        if(race==17)kim_ble_close();
        else {__atomic_add_fetch(&clock_ms,180001,__ATOMIC_SEQ_CST);kim_ble_tick();}
    }
    return acquired;
}
void kim_ble_resource_end(void){assert(__atomic_exchange_n(&resource_owner,0,__ATOMIC_SEQ_CST)==2);}
int os_mbuf_copydata(struct os_mbuf *m,int off,int n,void *out){memcpy(out,m->data+off,n);return 0;}
int os_mbuf_append(struct os_mbuf *m,const void *in,size_t n){memcpy(m->data+m->len,in,n);m->len+=n;return 0;}
int ble_gap_conn_find(uint16_t h,struct ble_gap_conn_desc *d){(void)h;*d=(struct ble_gap_conn_desc){.sec_state={1,1,security_key_size,security_bonded}};return 0;}
int ble_gatts_chr_updated(uint16_t h){(void)h;notify_count++;return 0;}
int ble_gap_adv_active(void){return 0;}
int ble_gap_adv_set_fields(struct ble_hs_adv_fields *f){(void)f;return 0;}
int ble_gap_adv_start(int t,void *a,int d,struct ble_gap_adv_params *p,int (*cb)(struct ble_gap_event*,void*),void *arg){(void)t;(void)a;(void)d;(void)p;(void)cb;(void)arg;return 0;}
static void event(int type,uint16_t handle,int status);
int ble_gap_terminate(uint16_t h,int reason){(void)reason;terminated=h;if(disconnect_on_terminate){disconnect_on_terminate=0;event(BLE_GAP_EVENT_DISCONNECT,h,0);}return 0;}
int ble_gap_security_initiate(uint16_t h){(void)h;return 0;}
int ble_sm_inject_io(uint16_t h,struct ble_sm_io *io){(void)h;(void)io;return 0;}
int ble_gap_adv_stop(void){return 0;}
int ble_hs_id_infer_auto(int x,uint8_t *type){(void)x;*type=0;return 0;}
int ble_svc_gap_device_name_set(const char *s){(void)s;return 0;}
void ble_svc_gap_init(void){} void ble_svc_gatt_init(void){}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *s){(void)s;return 0;}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *s){(void)s;return 0;}
uint32_t esp_random(void){return 123456789;}
int kim_ota_busy(void){return 0;} int kim_runtime_voice_busy(void){return 0;}
int kim_wifi_scan(void){return 0;} unsigned kim_wifi_scan_generation(void){return 1;}
int kim_wifi_scan_busy(void){return 0;} int kim_wifi_scan_results(kim_wifi_ap_t *out,int max){(void)out;(void)max;return 0;}
int kim_wifi_connected(void){return 0;} void kim_wifi_current_ssid(char out[33]){out[0]=0;}
int kim_wifi_save_tracked(const char *ssid,const char *password,unsigned *id){(void)ssid;assert(!strcmp(password,"synthetic123"));*id=7;return 0;}
int kim_wifi_forget_tracked(const char *ssid,unsigned *id){(void)ssid;*id=8;return 0;}
bool kim_wifi_operation_result(unsigned id,esp_err_t *err){(void)id;if(operation_ready){*err=wifi_result_error;return true;}return false;}
int kim_settings_network_count(void){return 0;} bool kim_settings_network_get(int i,kim_settings_network_t *out){(void)i;(void)out;return false;}
int kim_settings_volume(void){return 50;} int kim_settings_brightness(void){return 70;} int kim_settings_sleep_s(void){return 300;}
int kim_settings_set_volume(int v){(void)v;return 0;} int kim_settings_set_brightness(int v){(void)v;return 0;} int kim_settings_set_sleep_s(int v){(void)v;return 0;}
static void event(int type,uint16_t h,int status){struct ble_gap_event e={.type=type};
    if(type==BLE_GAP_EVENT_CONNECT){e.connect.conn_handle=h;e.connect.status=status;}
    else if(type==BLE_GAP_EVENT_DISCONNECT)e.disconnect.conn.conn_handle=h;
    else {e.enc_change.conn_handle=h;e.enc_change.status=status;}gap_cb(&e,NULL);
}
static void until_state(kim_ble_state_t s){for(int i=0;i<1000000;i++){if(kim_ble_state()==s)return;sched_yield();}assert(kim_ble_state()==s);}
static void start(void){assert(kim_ble_open()==ESP_OK);until_state(KIM_BLE_ACTIVE);assert(gate_held);}
static void stop(void){kim_ble_close();until_state(KIM_BLE_CLOSED);for(int i=0;i<100000 && gate_held;i++)sched_yield();assert(!gate_held && !requests);}
static void submit(uint16_t h,int id,int save){char text[200];snprintf(text,sizeof text,save?"{\"v\":1,\"id\":%d,\"op\":\"save\",\"ssid\":\"sample\",\"password\":\"synthetic123\"}":"{\"v\":1,\"id\":%d,\"op\":\"forget\",\"ssid\":\"sample\"}",id);
    struct os_mbuf mb={0};strcpy(mb.data,text);mb.len=strlen(text);struct ble_gatt_access_ctxt ctx={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&mb};assert(access_cb(h,0,&ctx,NULL)==0);
}
static void terminal_reply(int id){char match[40];snprintf(match,sizeof match,"\"id\":%d",id);for(int i=0;i<1000000;i++){if(!pending && strstr(reply,match))return;sched_yield();}assert(!pending && strstr(reply,match));}
static void admit(uint16_t h){event(BLE_GAP_EVENT_CONNECT,h,0);event(BLE_GAP_EVENT_ENC_CHANGE,h,0);assert(connection==h && paired);}
#ifdef STARTUP_TEST
int main(void){assert(kim_ble_init()==ESP_OK);kim_ble_tick();kim_ble_close();assert(!nimble_inits && !gate_held);
    manager_go=0;assert(kim_ble_open()==ESP_OK);until_state(KIM_BLE_PENDING);assert(!nimble_inits && !gate_held);
    kim_ble_close();manager_go=1;until_state(KIM_BLE_CLOSED);assert(!nimble_inits && !gate_held);
    puts("startup: pending and cancellation are nonblocking, no controller allocation");}
#elif defined(LIFECYCLE_TEST)
int main(void){assert(kim_ble_init()==0);start();admit(11);event(BLE_GAP_EVENT_CONNECT,12,0);event(BLE_GAP_EVENT_DISCONNECT,12,0);assert(connection==11 && paired);
    close_window(epoch-1,true);assert(kim_ble_active());
    submit(11,80,0);terminal_reply(80);disconnect_on_terminate=1;stop();assert(nimble_inits==nimble_deinits);
    host_stopped=0;start();admit(21);submit(21,81,1);terminal_reply(81);clock_ms+=180001;kim_ble_tick();until_state(KIM_BLE_CLOSED);assert(!gate_held);
    puts("lifecycle: rejected connection, synchronous disconnect and repeated deinit/reinit");}
#elif defined(FAULT_TEST)
int main(int argc,char **argv){assert(argc==2);int mode=atoi(argv[1]);assert(kim_ble_init()==ESP_OK);
    if(mode==1)fail_manager=1;
    if(mode==2)fail_queue=1;
    if(mode==3)fail_init=1;
    if(mode==4)fail_io=1;
    if(mode==5)fail_host=1;
    if(mode==6)wedge_stop=1;
    if(mode==7)fail_deinit=1;
    if(mode==8)gate_held=1;
    if(mode==9)fail_stop=1;
    if(mode==10)fail_stop=2;
    if(mode==11){fail_host=1;fail_deinit=1;}
    if(mode==12){assert(kim_tls_probe_begin());init_go=0;}
    if(mode==13){init_go=0;}
    if(mode==14){deinit_go=0;}
    if(mode>=15 && mode<=19){
        assert(kim_tls_probe_begin());
        if(mode==17 || mode==18)setup_race=mode;
    }
    if(mode==1){assert(kim_ble_open()==ESP_ERR_NO_MEM);assert(!nimble_inits && !gate_held && kim_ble_state()==KIM_BLE_FAILED);return 0;}
    assert(kim_ble_open()==ESP_OK);
    if(mode>=15 && mode<=19){
        for(int i=0;i<1000000 && !__atomic_load_n(&resource_attempts,__ATOMIC_SEQ_CST);i++)sched_yield();
        assert(resource_attempts && !nimble_inits && gate_held && resource_owner==1);
        if(mode==15)kim_ble_close();
        if(mode==16){__atomic_add_fetch(&clock_ms,180001,__ATOMIC_SEQ_CST);kim_ble_tick();}
        if(mode==19){
            for(int i=0;i<3000 && kim_ble_state()!=KIM_BLE_FAILED;i++){
                struct timespec pause={.tv_sec=0,.tv_nsec=1000000};nanosleep(&pause,NULL);
            }
            assert(kim_ble_state()==KIM_BLE_FAILED);
            assert(__atomic_load_n(&clock_ms,__ATOMIC_SEQ_CST)>=5100);
            for(int i=0;i<1000000 && manager_running;i++)sched_yield();
            assert(!manager_running && !gate_held && resource_owner==1 && !restarts && !nimble_inits);
            kim_tls_probe_end();assert(kim_tls_probe_begin());kim_tls_probe_end();return 0;
        }
        kim_tls_probe_end();until_state(KIM_BLE_CLOSED);
        for(int i=0;i<1000000 && manager_running;i++)sched_yield();
        assert(!manager_running && !gate_held && !resource_owner && !nimble_inits && !restarts && !requests);
        return 0;
    }
    if(mode==8){until_state(KIM_BLE_FAILED);assert(!nimble_inits && gate_held);return 0;}
    if(mode==11){for(int i=0;i<1000000 && !restarts;i++)sched_yield();
        assert(restarts && gate_held && !host_starts);return 0;}
    if(mode==3){for(int i=0;i<1000000 && !restarts;i++)sched_yield();
        assert(restarts && gate_held && resource_owner==2 && !nimble_deinits && nimble_inits==1);
        assert(kim_ble_open()==ESP_ERR_INVALID_STATE);return 0;}
    if(mode==12){for(int i=0;i<1000000 && !gate_held;i++)sched_yield();
        assert(!nimble_inits && !kim_tls_probe_begin());
        kim_tls_probe_end();for(int i=0;i<1000000 && !init_paused;i++)sched_yield();
        assert(init_paused && resource_owner==2 && !kim_tls_probe_begin());
        init_go=1;until_state(KIM_BLE_ACTIVE);
        for(int i=0;i<1000000 && resource_owner;i++)sched_yield();
        assert(kim_tls_probe_begin());kim_tls_probe_end();
        stop();return 0;}
    if(mode==13){for(int i=0;i<1000000 && !init_paused;i++)sched_yield();
        assert(resource_owner==2 && gate_held && !kim_tls_probe_begin());
        init_go=1;until_state(KIM_BLE_ACTIVE);stop();return 0;}
    if(mode==14){until_state(KIM_BLE_ACTIVE);for(int i=0;i<1000000 && resource_owner;i++)sched_yield();
        assert(kim_tls_probe_begin());kim_ble_close();
        for(int i=0;i<1000000 && !io_exited;i++)sched_yield();
        assert(!nimble_deinits && gate_held);
        kim_tls_probe_end();for(int i=0;i<1000000 && !deinit_paused;i++)sched_yield();
        assert(deinit_paused && resource_owner==2 && !kim_tls_probe_begin());
        deinit_go=1;until_state(KIM_BLE_CLOSED);assert(kim_tls_probe_begin());kim_tls_probe_end();return 0;}
    if(mode<=5){until_state(KIM_BLE_FAILED);
        for(int i=0;i<1000000 && manager_running;i++)sched_yield();
        assert(!manager_running && !gate_held && !requests && !restarts);
        assert(nimble_inits==0 || nimble_deinits==1);return 0;}
    until_state(KIM_BLE_ACTIVE);kim_ble_close();
    if(mode==6 || mode==7 || mode==9 || mode==10){for(int i=0;i<1000000 && !restarts;i++)sched_yield();
        assert(restarts && gate_held && kim_ble_state()==KIM_BLE_FAILED);
        if(mode==6 || mode==9 || mode==10)assert(nimble_deinits==0);return 0;}
    return 1;
}
#else
int main(void){assert(kim_ble_init()==0);start();admit(11);submit(11,1,1);terminal_reply(1);assert(strstr(reply,"\"state\":\"done\""));
    clock_ms+=300;wifi_result_error=ESP_ERR_INVALID_STATE;submit(11,2,0);terminal_reply(2);assert(strstr(reply,"\"state\":\"error\""));
    event(BLE_GAP_EVENT_CONNECT,12,0);event(BLE_GAP_EVENT_DISCONNECT,12,0);assert(connection==11 && paired);
    event(BLE_GAP_EVENT_DISCONNECT,11,0);admit(21);clock_ms+=300;submit(21,3,1);terminal_reply(3);stop();
    puts("runtime: real-C GATT/worker pending and terminal persistence");}
#endif
