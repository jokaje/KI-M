#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kim_ble_protocol.h"
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_INVALID_STATE 259
#define ESP_ERR_TIMEOUT 263
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
#define ESP_LOGI(...) ((void)0)
size_t heap_caps_get_free_size(int caps);
size_t heap_caps_get_largest_free_block(int caps);
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ATT_ERR_INSUFFICIENT_AUTHOR 8
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 5
#define BLE_ATT_ERR_INVALID_OFFSET 7
#define BLE_ATT_ERR_REQ_NOT_SUPPORTED 6
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_PREPARE_QUEUE_FULL 9
#define BLE_GATT_ACCESS_OP_READ_CHR 1
#define BLE_GATT_ACCESS_OP_WRITE_CHR 2
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_WRITE 1
#define BLE_GATT_CHR_F_WRITE_AUTHEN 2
#define BLE_GATT_CHR_F_READ 4
#define BLE_GATT_CHR_F_NOTIFY 8
#define BLE_GATT_CHR_F_READ_AUTHEN 16
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_ENC_CHANGE 3
#define BLE_GAP_EVENT_PASSKEY_ACTION 4
#define BLE_GAP_EVENT_REPEAT_PAIRING 5
#define BLE_GAP_REPEAT_PAIRING_IGNORE 1
#define BLE_ERR_REM_USER_CONN_TERM 1
#define BLE_ERR_AUTH_FAIL 2
#define BLE_SM_IOACT_DISP 1
#define BLE_SM_IO_CAP_DISP_ONLY 1
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 2
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
#define BLE_HS_FOREVER -1
#define pdTRUE 1
#define pdPASS 1
#define configMAX_PRIORITIES 25
#define NIMBLE_HS_STACK_SIZE 4096
#define NIMBLE_CORE 0
#define ESP_LOGE(...) ((void)0)
#define KIM_BLE_HOST_TEST 1
typedef enum { KIM_BLE_CLOSED, KIM_BLE_PENDING, KIM_BLE_ACTIVE,
               KIM_BLE_STOPPING, KIM_BLE_FAILED } kim_ble_state_t;
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(v) (v)
#define BLE_UUID128_INIT(...) {{0}}
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE *p);
void portEXIT_CRITICAL(portMUX_TYPE *p);
uint32_t xTaskGetTickCount(void);
void vTaskDelay(unsigned ms);
void vTaskDelete(void *task);
typedef struct test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned n, unsigned size);
void vQueueDelete(QueueHandle_t q);
int xQueueSend(QueueHandle_t q, const void *item, int timeout);
int xQueueReceive(QueueHandle_t q, void *item, unsigned timeout);
int xTaskCreate(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void *task);
int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void *task,int core);
typedef struct { struct {int dummy;} u; } ble_uuid128_t;
typedef struct { int encrypted, authenticated, key_size, bonded; } test_sec_t;
struct ble_gap_conn_desc { uint16_t conn_handle; test_sec_t sec_state; };
struct ble_gap_event { int type; union {
    struct {int status;uint16_t conn_handle;} connect;
    struct {struct ble_gap_conn_desc conn;int reason;} disconnect;
    struct {int status;uint16_t conn_handle;} enc_change;
    struct {uint16_t conn_handle;struct {int action;} params;} passkey;
}; };
struct os_mbuf { char data[600];size_t len; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
int os_mbuf_copydata(struct os_mbuf *om,int off,int n,void *out);
int os_mbuf_append(struct os_mbuf *om,const void *in,size_t n);
struct ble_gatt_access_ctxt {int op;size_t offset;struct os_mbuf *om;};
struct ble_gatt_chr_def {void *uuid;int (*access_cb)(uint16_t,uint16_t,struct ble_gatt_access_ctxt*,void*);uint16_t *val_handle;int flags;};
struct ble_gatt_svc_def {int type;void *uuid;struct ble_gatt_chr_def *characteristics;};
struct ble_hs_adv_fields {int flags;const uint8_t *name;int name_len,name_is_complete;};
struct ble_gap_adv_params {int conn_mode,disc_mode;};
struct ble_sm_io {int action;uint32_t passkey;};
struct {void (*sync_cb)(void);int sm_io_cap,sm_mitm,sm_sc,sm_sc_only,sm_bonding,sm_our_key_dist,sm_their_key_dist;} ble_hs_cfg;
int ble_gap_conn_find(uint16_t handle,struct ble_gap_conn_desc *desc);
int ble_gatts_chr_updated(uint16_t h);
int ble_gap_adv_active(void);
int ble_gap_adv_set_fields(struct ble_hs_adv_fields *f);
int ble_gap_adv_start(int type,void *addr,int duration,struct ble_gap_adv_params *p,int (*cb)(struct ble_gap_event*,void*),void *arg);
int ble_gap_terminate(uint16_t handle,int reason);
int ble_gap_security_initiate(uint16_t handle);
int ble_sm_inject_io(uint16_t handle,struct ble_sm_io *io);
int ble_gap_adv_stop(void);
int ble_hs_id_infer_auto(int x,uint8_t *type);
int ble_svc_gap_device_name_set(const char *name);
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *services);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *services);
int nimble_port_init(void);
int nimble_port_deinit(void);
int nimble_port_stop(void);
void nimble_port_run(void);
void esp_restart(void);
bool kim_maintenance_acquire(void);
void kim_maintenance_release(void);
bool kim_tls_probe_begin(void);
void kim_tls_probe_end(void);
bool kim_ble_resource_begin(void);
void kim_ble_resource_end(void);
void nimble_port_freertos_init(void (*fn)(void*));
void nimble_port_freertos_deinit(void);
uint32_t esp_random(void);
int kim_ota_busy(void);
int kim_runtime_voice_busy(void);
int kim_wifi_scan(void);
unsigned kim_wifi_scan_generation(void);
int kim_wifi_scan_busy(void);
typedef struct {char ssid[33];int rssi,saved,secure;} kim_wifi_ap_t;
int kim_wifi_scan_results(kim_wifi_ap_t *out,int max);
int kim_wifi_connected(void);
void kim_wifi_current_ssid(char out[33]);
int kim_wifi_save_tracked(const char *ssid,const char *password,unsigned *id);
int kim_wifi_forget_tracked(const char *ssid,unsigned *id);
bool kim_wifi_operation_result(unsigned id,esp_err_t *err);
typedef struct {char ssid[33],password[64];} kim_settings_network_t;
int kim_settings_network_count(void);
bool kim_settings_network_get(int index,kim_settings_network_t *out);
int kim_settings_volume(void);
int kim_settings_brightness(void);
int kim_settings_sleep_s(void);
int kim_settings_set_volume(int v);
int kim_settings_set_brightness(int v);
int kim_settings_set_sleep_s(int v);
