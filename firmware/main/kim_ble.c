/* KI-M-only, nonbonded, SC-only display-passkey provisioning. No cloud keys. */
#include "kim_ble.h"
#include "kim_ble_protocol.h"
#include "kim_wifi.h"
#include "kim_settings.h"
#include "kim_ota.h"
#include "kim_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_random.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "esp_system.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#define WINDOW_MS 180000U
#define IDLE_MS 30000U
#define REPLY_MAX 512
#define STOP_WAIT_MS 5000U
#define IO_WAIT_MS 12000U
/* 128-bit UUIDs little endian in NimBLE, canonical forms in BLE-HOMEHUB.md. */
static const ble_uuid128_t svc=BLE_UUID128_INIT(0x01,0x00,0x9e,0xe8,0x5b,0x64,0x31,0x84,0xa3,0x4e,0xad,0x62,0x00,0x00,0x4b,0x4d);
static const ble_uuid128_t cmd=BLE_UUID128_INIT(0x02,0x00,0x9e,0xe8,0x5b,0x64,0x31,0x84,0xa3,0x4e,0xad,0x62,0x00,0x00,0x4b,0x4d);
static const ble_uuid128_t status_uuid=BLE_UUID128_INIT(0x03,0x00,0x9e,0xe8,0x5b,0x64,0x31,0x84,0xa3,0x4e,0xad,0x62,0x00,0x00,0x4b,0x4d);
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t requests;
static uint8_t addr_type;
static uint16_t connection=BLE_HS_CONN_HANDLE_NONE, status_handle;
static bool initialized, synced, opened, paired, pending, cleaning;
static bool manager_running, io_exit_requested, io_exited, host_exited, stop_finished;
static int stop_result;
static kim_ble_state_t state=KIM_BLE_CLOSED;
static unsigned writers;
static uint32_t deadline, last_io, last_request, code;
static uint32_t epoch;
static bool code_visible;
static char reply[REPLY_MAX]="{\"v\":1,\"id\":0,\"state\":\"closed\"}";
static int gap_cb(struct ble_gap_event *e, void *arg);
static void enter(void) { portENTER_CRITICAL(&lock); }
static void leave(void) { portEXIT_CRITICAL(&lock); }
static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS); }
static bool expired(uint32_t now,uint32_t end) { return (int32_t)(now-end)>=0; }
static void heap_counts(const char *phase) {
    ESP_LOGI("kim-ble", "%s heap internal free=%u largest=%u psram free=%u largest=%u", phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
static void publish(cJSON *obj, uint32_t session) {
    char *text=obj ? cJSON_PrintUnformatted(obj) : NULL;
    const cJSON *id=obj ? cJSON_GetObjectItemCaseSensitive(obj,"id") : NULL;
    enter();
    bool notify=session==epoch && opened && !cleaning && !expired(now_ms(),deadline) &&
                connection!=BLE_HS_CONN_HANDLE_NONE;
    uint16_t target=connection;
    if(notify) {
        if(!text || strlen(text)>=sizeof reply)
            snprintf(reply,sizeof reply,"{\"v\":1,\"id\":%u,\"state\":\"error\",\"code\":%d}",
                     id && cJSON_IsNumber(id) ? (unsigned)id->valueint : 0U, ESP_ERR_NO_MEM);
        else strcpy(reply,text);
        pending=false;
    }
    leave();
    if(notify) {
        enter(); notify=session==epoch && opened && !cleaning && connection==target; leave();
        if(notify && status_handle) ble_gatts_chr_updated(status_handle);
    }
    if(text) { memset(text,0,strlen(text)); free(text); }
    cJSON_Delete(obj);
}
static cJSON *base(uint32_t id,const char *state) {
    cJSON *r=cJSON_CreateObject();
    if(r) { cJSON_AddNumberToObject(r,"v",1); cJSON_AddNumberToObject(r,"id",id); cJSON_AddStringToObject(r,"state",state); }
    return r;
}
static bool live(void) { enter(); bool ok=state==KIM_BLE_ACTIVE && opened && !expired(now_ms(),deadline); leave(); return ok; }
static int access_cb(uint16_t handle,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg) {
    (void)attr; (void)arg;
    struct ble_gap_conn_desc desc;
    enter(); bool allowed=opened && !expired(now_ms(),deadline) && handle==connection && paired;
    leave();
    if (!allowed) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ble_gap_conn_find(handle,&desc) || !desc.sec_state.encrypted ||
        !desc.sec_state.authenticated || desc.sec_state.key_size!=16 || desc.sec_state.bonded)
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    enter(); allowed=opened && handle==connection && paired && !expired(now_ms(),deadline);
    if(allowed) last_io=now_ms();
    leave();
    if(!allowed) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if(ctxt->op==BLE_GATT_ACCESS_OP_READ_CHR) {
        char snapshot[REPLY_MAX]; enter(); strcpy(snapshot,reply); leave();
        size_t n=strlen(snapshot);
        if(ctxt->offset>n) return BLE_ATT_ERR_INVALID_OFFSET;
        /* NimBLE ble_gatts_val_access calls back for the FULL value and
         * applies offset itself; pre-slicing here would double-apply it. */
        return os_mbuf_append(ctxt->om,snapshot,n)==0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if(ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    size_t n=OS_MBUF_PKTLEN(ctxt->om);
    if(!n || n>KIM_BLE_MAX_REQUEST) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    char raw[KIM_BLE_MAX_REQUEST];
    if(os_mbuf_copydata(ctxt->om,0,n,raw)) return BLE_ATT_ERR_UNLIKELY;
    kim_ble_request_t request;
    int valid=kim_ble_parse(raw,n,&request);
    memset(raw,0,sizeof raw);
    if(valid!=KIM_BLE_VALID) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    enter(); bool busy=cleaning || !opened || handle!=connection || !paired || pending || (last_request && !expired(now_ms(),last_request+250));
    if(!busy) { pending=true; ++writers; request.session=epoch; last_request=now_ms(); } leave();
    if(busy) { kim_ble_wipe(&request); return BLE_ATT_ERR_PREPARE_QUEUE_FULL; }
    cJSON *queued=base(request.id,"pending");
    char *text=queued ? cJSON_PrintUnformatted(queued) : NULL;
    if(!text || strlen(text)>=sizeof reply) {
        if(text) free(text);
        cJSON_Delete(queued);
        enter(); if(request.session==epoch) pending=false; --writers; leave(); kim_ble_wipe(&request);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    /* A close owns cleanup only after every admitted writer has finished sending.
     * Never hold the state spinlock while invoking a NimBLE notification. */
    enter(); bool current=request.session==epoch && opened && !cleaning && handle==connection;
    if(current) strcpy(reply,text);
    leave();
    int sent=current ? xQueueSend(requests,&request,0) : 0;
    enter();
    current=current && request.session==epoch && opened && !cleaning && handle==connection;
    if(sent!=pdTRUE && request.session==epoch) { pending=false; reply[0]=0; }
    --writers;
    leave();
    free(text); cJSON_Delete(queued);
    uint32_t session=request.session;
    kim_ble_wipe(&request);
    if(sent!=pdTRUE || !current) return BLE_ATT_ERR_INSUFFICIENT_RES;
    enter(); bool notify=opened && !cleaning && handle==connection && session==epoch; leave();
    if(notify && status_handle) ble_gatts_chr_updated(status_handle);
    return 0;
}
static const struct ble_gatt_svc_def services[]={
 { .type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&svc.u,.characteristics=(struct ble_gatt_chr_def[]){
   {.uuid=&cmd.u,.access_cb=access_cb,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_AUTHEN},
   {.uuid=&status_uuid.u,.access_cb=access_cb,.val_handle=&status_handle,
    .flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY|BLE_GATT_CHR_F_READ_AUTHEN}, {0}} },{0}};
static void worker(void *unused) {
    (void)unused;
    kim_ble_request_t req;
    for(;;) {
        if(xQueueReceive(requests,&req,pdMS_TO_TICKS(20))!=pdTRUE) {
            enter(); bool quit=io_exit_requested; leave();
            if(quit) break;
            continue;
        }
        esp_err_t err=ESP_OK;
        cJSON *r=base(req.id,"done");
        if(!r) { enter(); if(req.session==epoch) pending=false; leave(); kim_ble_wipe(&req); continue; }
        enter(); bool same=req.session==epoch; leave();
        if(!same || !live() || kim_ota_busy() || kim_runtime_voice_busy()) err=ESP_ERR_INVALID_STATE;
        else if(!strcmp(req.op,"scan")) {
            unsigned gen=kim_wifi_scan_generation();
            err=kim_wifi_scan();
            if(err==ESP_OK) {
                uint32_t until=now_ms()+16000;
                while(kim_wifi_scan_generation()==gen && kim_wifi_scan_busy() && !expired(now_ms(),until)) vTaskDelay(pdMS_TO_TICKS(50));
                if(kim_wifi_scan_generation()==gen) err=ESP_ERR_TIMEOUT;
            }
            if(err==ESP_OK) {
                cJSON *aps=cJSON_AddArrayToObject(r,"aps");
                kim_wifi_ap_t found[4]; int count=kim_wifi_scan_results(found,4);
                for(int i=0;i<count;i++) {
                    cJSON *ap=cJSON_CreateObject(); cJSON_AddStringToObject(ap,"ssid",found[i].ssid);
                    cJSON_AddNumberToObject(ap,"rssi",found[i].rssi);
                    cJSON_AddBoolToObject(ap,"saved",found[i].saved);
                    cJSON_AddBoolToObject(ap,"secure",found[i].secure); cJSON_AddItemToArray(aps,ap);
                }
            }
        } else if(!strcmp(req.op,"networks")) {
            cJSON *aps=cJSON_AddArrayToObject(r,"saved");
            kim_settings_network_t net;
            for(int i=0;i<kim_settings_network_count();i++) if(kim_settings_network_get(i,&net)) {
                cJSON_AddItemToArray(aps,cJSON_CreateString(net.ssid)); memset(&net,0,sizeof net);
            }
            char current[33]; kim_wifi_current_ssid(current);
            cJSON_AddStringToObject(r,"current",kim_wifi_connected()?current:"");
            cJSON_AddBoolToObject(r,"connected",kim_wifi_connected());
        } else if(!strcmp(req.op,"settings_get")) {
            cJSON_AddNumberToObject(r,"volume",kim_settings_volume());
            cJSON_AddNumberToObject(r,"brightness",kim_settings_brightness());
            cJSON_AddNumberToObject(r,"sleep_s",kim_settings_sleep_s());
        } else if(!strcmp(req.op,"settings_set")) {
            if(req.fields&(1U<<5)) err=kim_settings_set_volume(req.volume);
            else if(req.fields&(1U<<6)) err=kim_settings_set_brightness(req.brightness);
            else err=kim_settings_set_sleep_s(req.sleep_s);
        } else {
            unsigned id=0;
            err=!strcmp(req.op,"save") ? kim_wifi_save_tracked(req.ssid,req.password,&id) :
                                         kim_wifi_forget_tracked(req.ssid,&id);
            /* Wi-Fi worker copied the secret; keep only the request identity. */
            memset(req.password,0,sizeof req.password);
            if(err==ESP_OK) {
                uint32_t until=now_ms()+10000;
                while(!kim_wifi_operation_result(id,&err) && !expired(now_ms(),until)) vTaskDelay(pdMS_TO_TICKS(20));
                if(!kim_wifi_operation_result(id,&err)) err=ESP_ERR_TIMEOUT;
            }
        }
        uint32_t session=req.session;
        kim_ble_wipe(&req);
        if(err!=ESP_OK) { cJSON_ReplaceItemInObjectCaseSensitive(r,"state",cJSON_CreateString("error")); cJSON_AddNumberToObject(r,"code",err); }
        publish(r,session);
    }
    enter(); io_exited=true; leave();
    vTaskDelete(NULL);
}
static void advertise(void) {
    enter(); bool can=synced && opened && !cleaning && !expired(now_ms(),deadline) && connection==BLE_HS_CONN_HANDLE_NONE;
    uint8_t type=addr_type; leave();
    if(!can || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields={0};
    fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name=(const uint8_t *)"KI-M Setup"; fields.name_len=10; fields.name_is_complete=1;
    if(ble_gap_adv_set_fields(&fields)) return;
    struct ble_gap_adv_params params={0}; params.conn_mode=BLE_GAP_CONN_MODE_UND; params.disc_mode=BLE_GAP_DISC_MODE_GEN;
    enter(); can=synced && opened && !cleaning && !expired(now_ms(),deadline) && connection==BLE_HS_CONN_HANDLE_NONE; leave();
    if(can && !ble_gap_adv_start(type,NULL,BLE_HS_FOREVER,&params,gap_cb,NULL)) {
        enter(); bool stop=cleaning || !opened || expired(now_ms(),deadline) || connection!=BLE_HS_CONN_HANDLE_NONE;
        leave();
        if(stop) ble_gap_adv_stop();
    }
}
static void drain_requests(void) {
    /* Admission was disabled under lock before this wait. A GATT writer never
     * invokes NimBLE while counted, so cleanup can safely join its queue send. */
    for(;;) {
        enter(); bool writing=writers!=0; leave();
        if(!writing) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    kim_ble_request_t dropped;
    while(requests && xQueueReceive(requests,&dropped,0)==pdTRUE) kim_ble_wipe(&dropped);
}
static int gap_cb(struct ble_gap_event *e,void *arg) {
    (void)arg;
    switch(e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(e->connect.status==0) {
            enter(); bool admit=opened && !cleaning && !expired(now_ms(),deadline) && connection==BLE_HS_CONN_HANDLE_NONE;
            if(admit) { connection=e->connect.conn_handle; paired=false; }
            leave();
            if(admit) {
                if(ble_gap_security_initiate(e->connect.conn_handle))
                    ble_gap_terminate(e->connect.conn_handle,BLE_ERR_AUTH_FAIL);
            } else ble_gap_terminate(e->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        enter(); bool ours=connection==e->disconnect.conn.conn_handle;
        if(ours) {
            connection=BLE_HS_CONN_HANDLE_NONE; paired=false; ++epoch;
            code_visible=false; code=0; reply[0]=0; pending=false; last_request=0;
        }
        bool owner=ours && !cleaning;
        if(owner) cleaning=true;
        leave();
        if(owner) {
            drain_requests();
            /* Disconnect cleanup always finishes host advertising before admission
             * reopens; a concurrent close only marks the window closed. */
            ble_gap_adv_stop();
            enter(); cleaning=false; leave();
            advertise();
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        enter(); bool ours=e->enc_change.conn_handle==connection && opened && !expired(now_ms(),deadline);
        leave();
        if(!ours) break;
        struct ble_gap_conn_desc d;
        bool secure=e->enc_change.status==0 && !ble_gap_conn_find(e->enc_change.conn_handle,&d) &&
               d.sec_state.encrypted && d.sec_state.authenticated && d.sec_state.key_size==16 && !d.sec_state.bonded;
        enter(); ours=e->enc_change.conn_handle==connection && opened && !expired(now_ms(),deadline);
        if(ours) { paired=secure; code_visible=false; code=0; }
        leave();
        if(ours && !secure) ble_gap_terminate(e->enc_change.conn_handle,BLE_ERR_AUTH_FAIL);
        break;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        enter(); bool ours=e->passkey.conn_handle==connection && opened && !expired(now_ms(),deadline);
        leave();
        if(e->passkey.params.action!=BLE_SM_IOACT_DISP || !ours) {
            ble_gap_terminate(e->passkey.conn_handle,BLE_ERR_AUTH_FAIL); break;
        }
        /* Rejection-sampled uniform six-digit passkey; never log it. */
        uint32_t random; do { random=esp_random(); } while(random>=UINT32_MAX-(UINT32_MAX%900000));
        uint32_t pin=100000+random%900000;
        enter(); ours=e->passkey.conn_handle==connection && opened && !expired(now_ms(),deadline);
        if(ours) { code=pin; code_visible=true; }
        leave();
        if(!ours) { ble_gap_terminate(e->passkey.conn_handle,BLE_ERR_AUTH_FAIL); break; }
        struct ble_sm_io io={.action=BLE_SM_IOACT_DISP,.passkey=pin};
        if(ble_sm_inject_io(e->passkey.conn_handle,&io)) ble_gap_terminate(e->passkey.conn_handle,BLE_ERR_AUTH_FAIL);
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default: break;
    }
    return 0;
}
static void sync_cb(void) {
    uint8_t type;
    if(!ble_hs_id_infer_auto(0,&type)) { enter(); addr_type=type; synced=true; leave(); advertise(); }
}
static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    enter(); host_exited=true; leave();
    vTaskDelete(NULL);
}
static void stop_task(void *arg) {
    (void)arg;
    int rc=nimble_port_stop();
    enter(); stop_result=rc; stop_finished=true; leave();
    vTaskDelete(NULL);
}
static bool wait_flag(bool *flag, uint32_t timeout) {
    uint32_t until=now_ms()+timeout;
    for(;;) {
        enter(); bool done=*flag; leave();
        if(done) return true;
        if(expired(now_ms(),until)) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
/* This is deliberately called ONLY from the manager; never deinit a live host.
 * A wedged stop or failed deinit leaves the maintenance gate held until reset. */
static void fail_closed(const char *phase) {
    ESP_LOGE("kim-ble", "%s; controlled restart with maintenance held",phase);
    enter(); state=KIM_BLE_FAILED; opened=false; leave();
    esp_restart();
    for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void manager_task(void *arg);
esp_err_t kim_ble_init(void) {
    /* Application startup does not claim controller, host pools or task stacks.
     * The menu's physical Start action alone calls start_host(). */
    return ESP_OK;
}
static esp_err_t start_host(void) {
    heap_counts("before setup");
    requests=xQueueCreate(1,sizeof(kim_ble_request_t));
    if(!requests) return ESP_ERR_NO_MEM;
    esp_err_t err=nimble_port_init();
    /* IDF may retain NPL allocations on partial init failure. No safe
     * deinit contract exists here; reboot without reopening the gate. */
    if(err!=ESP_OK) fail_closed("NimBLE partial init failed");
    initialized=true;
    ble_hs_cfg.sync_cb=sync_cb;
    ble_hs_cfg.sm_io_cap=BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_mitm=1; ble_hs_cfg.sm_sc=1; ble_hs_cfg.sm_sc_only=1;
    ble_hs_cfg.sm_bonding=0; ble_hs_cfg.sm_our_key_dist=0; ble_hs_cfg.sm_their_key_dist=0;
    ble_svc_gap_init(); ble_svc_gatt_init();
    if(ble_svc_gap_device_name_set("KI-M Setup") || ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) { err=ESP_FAIL; goto failed_host; }
    if(xTaskCreate(worker,"kim-ble-io",8192,NULL,3,NULL)!=pdPASS) { err=ESP_ERR_NO_MEM; goto failed_host; }
    if(xTaskCreatePinnedToCore(host_task,"nimble_host",NIMBLE_HS_STACK_SIZE,
                              NULL,configMAX_PRIORITIES-4,NULL,NIMBLE_CORE)!=pdPASS) {
        err=ESP_ERR_NO_MEM;
        enter(); io_exit_requested=true; leave();
        if(!wait_flag(&io_exited,IO_WAIT_MS)) fail_closed("IO worker failed to exit");
        goto failed_host;
    }
    heap_counts("after setup");
    return ESP_OK;
failed_host:
    if(nimble_port_deinit()!=ESP_OK) fail_closed("partial init deinit failed");
    initialized=false;
    vQueueDelete(requests); requests=NULL;
    return err;
}
static void manager_task(void *arg) {
    (void)arg;
    enter(); uint32_t session=epoch;
    bool wanted=state==KIM_BLE_PENDING && !expired(now_ms(),deadline); leave();
    if(!wanted) { kim_ble_tick(); goto cancelled; }
    if(!kim_maintenance_acquire()) {
        enter(); state=KIM_BLE_FAILED; leave();
        goto cancelled;
    }
    /* No controller, host pool, queue or BLE worker allocation precedes the gate. */
    enter(); wanted=state==KIM_BLE_PENDING && epoch==session && !expired(now_ms(),deadline); leave();
    if(!wanted) { kim_ble_tick(); goto release_gate; }
    /* Lock order: maintenance -> resource admission -> BLE allocations.
     * No state spinlock is held during the bounded wait. */
    uint32_t resource_until=now_ms()+STOP_WAIT_MS;
    for(;;) {
        /* The TLS probe owns its admission until completion. We own only the
         * maintenance gate while waiting; never withdraw another owner's probe. */
        enter(); wanted=state==KIM_BLE_PENDING && epoch==session && !expired(now_ms(),deadline);
        leave();
        if(!wanted) { kim_ble_tick(); goto release_gate; }
        if(expired(now_ms(),resource_until)) {
            enter();
            if(state==KIM_BLE_PENDING && epoch==session && !expired(now_ms(),deadline))
                state=KIM_BLE_FAILED;
            leave();
            kim_ble_tick();
            ESP_LOGI("kim-ble","setup busy: TLS admission wait elapsed before BLE allocation");
            goto release_gate;
        }
        if(kim_ble_resource_begin()) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Acquisition is not consent: close/expiry can interleave with the CAS. */
    enter(); wanted=state==KIM_BLE_PENDING && epoch==session && !expired(now_ms(),deadline); leave();
    if(!wanted || expired(now_ms(),resource_until)) {
        kim_ble_resource_end(); kim_ble_tick();
        if(wanted) {
            enter(); if(state==KIM_BLE_PENDING && epoch==session) state=KIM_BLE_FAILED; leave();
        }
        goto release_gate;
    }
    esp_err_t err=start_host();
    if(err!=ESP_OK) {
        ESP_LOGE("kim-ble","setup failed: %d",err);
        enter(); state=KIM_BLE_FAILED; leave();
        kim_ble_resource_end();
        goto release_gate;
    }
    /* A created task is not a synchronized host. Never expose ACTIVE early. */
    uint32_t sync_until=now_ms()+STOP_WAIT_MS;
    for(;;) {
        enter(); bool ready=synced, cancelled=state!=KIM_BLE_PENDING || expired(now_ms(),deadline); leave();
        if(ready || cancelled || expired(now_ms(),sync_until)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    enter();
    wanted=synced && state==KIM_BLE_PENDING && !expired(now_ms(),deadline);
    if(wanted) { state=KIM_BLE_ACTIVE; opened=true; }
    else if(state==KIM_BLE_PENDING) state=KIM_BLE_STOPPING;
    leave();
    kim_ble_resource_end(); /* synchronized pools are stable while ACTIVE */
    if(wanted) advertise();
    while(wanted) {
        vTaskDelay(pdMS_TO_TICKS(20));
        enter(); bool unexpected_exit=host_exited && state==KIM_BLE_ACTIVE;
        wanted=state==KIM_BLE_ACTIVE && !expired(now_ms(),deadline) &&
                        (connection==BLE_HS_CONN_HANDLE_NONE || !expired(now_ms(),last_io+IDLE_MS));
        leave();
        if(unexpected_exit) fail_closed("NimBLE host exited before stop");
        if(!wanted) kim_ble_close();
    }
    /* Shut admission before joining writers. Synchronous disconnect is an observer. */
    enter(); opened=false; cleaning=true; paired=false; ++epoch;
    code=0; code_visible=false; pending=false; reply[0]=0;
    uint16_t handle=connection;
    leave();

    /* GATT callbacks can be paused during their queue send; join before drain. */
    uint32_t until=now_ms()+IO_WAIT_MS;
    for(;;) {
        enter(); bool writing=writers!=0; leave();
        if(!writing) break;
        if(expired(now_ms(),until)) fail_closed("GATT writer did not exit");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    drain_requests();
    enter(); io_exit_requested=true; leave();
    if(!wait_flag(&io_exited,IO_WAIT_MS)) fail_closed("IO worker did not exit");
    resource_until=now_ms()+STOP_WAIT_MS;
    while(!kim_ble_resource_begin()) {
        if(expired(now_ms(),resource_until)) fail_closed("TLS probe held BLE teardown resources");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ble_gap_adv_stop();
    if(handle!=BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);
    if(xTaskCreate(stop_task,"kim-ble-stop",3072,NULL,4,NULL)!=pdPASS)
        fail_closed("stop helper allocation failed");
    if(!wait_flag(&stop_finished,STOP_WAIT_MS)) fail_closed("NimBLE stop timed out");
    if(stop_result!=0 || !wait_flag(&host_exited,STOP_WAIT_MS)) fail_closed("NimBLE host did not exit");
    if(nimble_port_deinit()!=ESP_OK) fail_closed("NimBLE deinit failed");
    initialized=false;
    vQueueDelete(requests); requests=NULL;
    /* FreeRTOS idle may still own self-deleted task stacks at this point. */
    heap_counts("after BLE deinit (task reclamation pending)");
    enter(); synced=false; connection=BLE_HS_CONN_HANDLE_NONE; cleaning=false;
    io_exit_requested=false; io_exited=false; host_exited=false; stop_finished=false;
    leave();
    kim_ble_resource_end();
release_gate:
    kim_maintenance_release();
cancelled:
    enter();
    if(state==KIM_BLE_STOPPING) state=KIM_BLE_CLOSED;
    manager_running=false;
    leave();
    vTaskDelete(NULL);
}
esp_err_t kim_ble_open(void) {
    enter();
    if(manager_running || (state!=KIM_BLE_CLOSED && state!=KIM_BLE_FAILED)) { leave(); return ESP_ERR_INVALID_STATE; }
    manager_running=true; state=KIM_BLE_PENDING;
    opened=false; paired=false; ++epoch; deadline=now_ms()+WINDOW_MS;
    last_io=now_ms(); last_request=0; pending=false; code_visible=false; code=0; reply[0]=0;
    leave();
    if(xTaskCreate(manager_task,"kim-ble-manager",4096,NULL,4,NULL)!=pdPASS) {
        enter(); manager_running=false; state=KIM_BLE_FAILED; leave();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static void close_window(uint32_t expected, bool conditional) {
    enter();
    if(state!=KIM_BLE_ACTIVE && state!=KIM_BLE_PENDING) { leave(); return; }
    if(conditional && epoch!=expected) { leave(); return; }
    opened=false; paired=false; ++epoch; code_visible=false; code=0; state=KIM_BLE_STOPPING;
    reply[0]=0; pending=false;
    cleaning=true;
    leave();
}
void kim_ble_close(void) { close_window(0,false); }
void kim_ble_tick(void) {
    enter(); bool close=(state==KIM_BLE_PENDING || state==KIM_BLE_ACTIVE) && (expired(now_ms(),deadline) ||
        (connection!=BLE_HS_CONN_HANDLE_NONE && expired(now_ms(),last_io+IDLE_MS)));
    uint32_t session=epoch;
    leave(); if(close) close_window(session,true);
}
bool kim_ble_active(void) { return live(); }
kim_ble_state_t kim_ble_state(void) { enter(); kim_ble_state_t s=state; leave(); return s; }
bool kim_ble_passkey(char out[7]) {
    if(!out) return false;
    enter(); bool show=code_visible && opened && !expired(now_ms(),deadline) && connection!=BLE_HS_CONN_HANDLE_NONE;
    if(show) snprintf(out,7,"%06lu",(unsigned long)code);
    leave(); return show;
}
