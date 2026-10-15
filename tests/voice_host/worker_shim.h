#pragma once
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#include <assert.h>
#include "audio_jitter.h"
#include "pcm_stream.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 100000
#define KIM_SERVER_URL "https://example.org/v1/turn"
#define KIM_SERVER_CA_PEM ""
#define KIM_DEVICE_TOKEN "host-test-nonsecret"
#define ESP_OK 0
#define ESP_ERR_HTTP_EAGAIN 11
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_METHOD_POST 1
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef pthread_t TaskHandle_t;
typedef struct { pthread_mutex_t mutex; atomic_bool signaled; bool binary; } shim_sem_t;
typedef shim_sem_t *SemaphoreHandle_t;
typedef struct { int event_id; const char *header_key, *header_value; void *user_data; } esp_http_client_event_t;
typedef struct { const char *url, *cert_pem; void *crt_bundle_attach; int timeout_ms, buffer_size; esp_err_t (*event_handler)(esp_http_client_event_t *); void *user_data; } esp_http_client_config_t;
typedef struct { esp_http_client_config_t cfg; int pos; } shim_http_t;
typedef shim_http_t *esp_http_client_handle_t;
extern int mock_status, mock_alloc_fail, mock_create_fail, mock_write_delay_us, mock_body_mode;
extern int mock_reset_exit;
extern atomic_int mock_closed, mock_codec_writes, mock_http_fallback, mock_restarts;
extern atomic_bool mock_mute;
extern uint8_t mock_body[8192];
extern int mock_body_size;
static inline void *heap_caps_calloc(size_t n, size_t z, int c) { (void)c; return mock_alloc_fail ? NULL : calloc(n,z); }
static inline void *heap_caps_malloc(size_t n, int c) { (void)c; return mock_alloc_fail ? NULL : malloc(n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { shim_sem_t *s=calloc(1,sizeof(*s)); if(s) pthread_mutex_init(&s->mutex,NULL); return s; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { shim_sem_t *s=xSemaphoreCreateMutex(); if(s) s->binary=true; return s; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, int ticks) { if(!s->binary) return pthread_mutex_lock(&s->mutex)==0; for(int i=0;i<ticks;i++){if(atomic_exchange(&s->signaled,false))return pdTRUE; struct timespec d={0,1000000}; nanosleep(&d,NULL);} return 0; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { if(s->binary)atomic_store(&s->signaled,true);else pthread_mutex_unlock(&s->mutex);return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s){pthread_mutex_destroy(&s->mutex);free(s);}
static inline int xTaskCreate(void (*fn)(void*),const char *name,int stack,void *arg,int priority,TaskHandle_t *task) { (void)name;(void)stack;(void)priority; if(mock_create_fail)return 0; extern void *worker_entry(void*); (void)fn; return pthread_create(task,NULL,worker_entry,arg)==0 ? pdPASS:0; }
static inline int xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,int stack,void *arg,int priority,TaskHandle_t *task,int core) { return xTaskCreate(fn,name,stack,arg,priority,task); }
static inline void vTaskDelete(void *arg) { (void)arg; pthread_exit(NULL); }
static inline void esp_restart(void) { atomic_fetch_add(&mock_restarts,1); if(mock_reset_exit) { assert(atomic_load(&mock_closed)==0); _exit(42); } abort(); }
static void *esp_crt_bundle_attach;
static inline void kim_trial_reply(kim_trial_t *t,bool nonzero,bool complete){(void)t;(void)nonzero;(void)complete;}
static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) { shim_http_t *h=calloc(1,sizeof(*h));if(h)h->cfg=*cfg;return h; }
static inline void esp_http_client_set_method(esp_http_client_handle_t h,int m){(void)h;(void)m;}
static inline void esp_http_client_set_header(esp_http_client_handle_t h,const char *a,const char *b){(void)h;(void)a;(void)b;}
static inline esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n){(void)h;(void)n;return ESP_OK;}
static inline int esp_http_client_write(esp_http_client_handle_t h,const char *s,int n){(void)h;(void)s;return n;}
static inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) { if(mock_status==200){esp_http_client_event_t e={HTTP_EVENT_ON_HEADER,"Content-Type","application/vnd.kim.stream-v1",h->cfg.user_data};h->cfg.event_handler(&e);} return 0; }
static inline int esp_http_client_get_status_code(esp_http_client_handle_t h){return strstr(h->cfg.url,"turn-stream") ? mock_status : 200;}
static inline int esp_http_client_get_content_length(esp_http_client_handle_t h){(void)h;return mock_body_size;}
static inline bool esp_http_client_is_chunked_response(esp_http_client_handle_t h){(void)h;return false;}
static inline int esp_http_client_read(esp_http_client_handle_t h,char *b,int n){ if(h->pos>=mock_body_size)return 0;int take=mock_body_size-h->pos;if(take>n)take=n;memcpy(b,mock_body+h->pos,take);h->pos+=take;return take;}
static inline void esp_http_client_close(esp_http_client_handle_t h){(void)h;atomic_fetch_add(&mock_closed,1);}
static inline void esp_http_client_cleanup(esp_http_client_handle_t h){free(h);}
