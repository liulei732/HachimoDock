/* Hardware adapters for the real community worker. The host controls when a
 * network task runs and when the protocol worker acquires its storage lane. */
#include "pet_p4_community.h"
#include "pet_p4_miniapp.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void (*task)(void *);
static void *task_arg, *queued;
static bool online=true;
static unsigned nonce;
typedef int (*get_t)(const char *,void *,size_t);
static get_t get;
void test_set_http(get_t fn) { get=fn; }
void test_online(bool on) { online=on; }
bool pet_device_wifi_online(void) { return online; }
void esp_fill_random(void *out,size_t n) { memset(out,++nonce,n); }
int xTaskCreate(void (*fn)(void *), const char *name,unsigned stack,void *arg,unsigned priority,void *id) {
  (void)name;(void)stack;(void)priority;(void)id;
  assert(!task);
  task=fn; task_arg=arg;
  return 1;
}
void vTaskDelete(void *unused) { (void)unused; }
void vTaskDelay(uint32_t ms) { (void)ms; assert(false); }
bool pet_community_schedule(void *job) { assert(!queued); queued=job; return true; }
/* The production GET writer and bounded binary response reader run over a
 * fragmented TLS-stream adapter. The live probe connects the adapter to the
 * isolated, certificate-verified HTTPS service. */
#include "pet_p4_community_config.h"
#include "pet_p4_wifi_http.h"
#include "mbedtls/platform_util.h"
#include <time.h>
#include <sys/types.h>
#define PET_WIFI_DATA_URL_MAX 512
#ifndef PET_PRIVATE_TOKEN_MAX
#define PET_PRIVATE_TOKEN_MAX 128
#endif
#define MIN_TLS_EPOCH 1704067200
#define ESP_TLS_ERR_SSL_WANT_READ -1
#define ESP_TLS_ERR_SSL_WANT_WRITE -2
static char receive_buffer[1024];
typedef struct {bool non_block; int timeout_ms,crt_bundle_attach; const uint8_t *cacert_buf; size_t cacert_bytes;} esp_tls_cfg_t;
static const int esp_crt_bundle_attach=1;
typedef struct {char request[2048],response[8512]; size_t written,size,pos;} esp_tls_t;
typedef struct {esp_tls_t *tls;uint64_t deadline;size_t cursor,available;char *private_buffer;bool (*current)(void *);void *context;} https_reader_t;
static unsigned wire_fault;
void test_wire_fault(unsigned fault){wire_fault=fault;}
static uint64_t now_ms(void){return 1000;}
static esp_tls_t *pet_live_tls_connect_port(const char *host,unsigned port,const esp_tls_cfg_t *cfg,
    uint64_t deadline,bool (*live)(void *),void *ctx) {
  assert(!strcmp(host,PET_COMMUNITY_HOST) && port==PET_COMMUNITY_PORT && cfg->non_block && deadline>now_ms());
#ifdef PET_COMMUNITY_CA_PEM
  assert(cfg->cacert_bytes==sizeof(PET_COMMUNITY_CA_PEM) && cfg->cacert_buf &&
         !memcmp(cfg->cacert_buf,PET_COMMUNITY_CA_PEM,cfg->cacert_bytes));
#else
  assert(cfg->crt_bundle_attach==esp_crt_bundle_attach);
#endif
  return live(ctx)?calloc(1,sizeof(esp_tls_t)):NULL;
}
static void esp_tls_conn_destroy(esp_tls_t *tls){free(tls);}
static ssize_t esp_tls_conn_write(esp_tls_t *tls,const char *data,size_t n) {
  if (n>17) n=17;
  assert(tls->written+n<sizeof(tls->request));
  memcpy(tls->request+tls->written,data,n);
  tls->written+=n;
  return n;
}
static ssize_t esp_tls_conn_read(esp_tls_t *tls,char *out,size_t n) {
  if (!tls->size) {
    char path[256]={0},payload[8193];
    assert(sscanf(tls->request,"GET %255s HTTP/1.1",path)==1 && strstr(tls->request,"\r\n\r\n"));
    char authority[300];
    snprintf(authority,sizeof(authority),"\r\nHost: %s:%u\r\n",PET_COMMUNITY_HOST,(unsigned)PET_COMMUNITY_PORT);
    assert(strstr(tls->request,authority));
    int bytes=get(path,payload,sizeof(payload)-1);
    if (bytes<0) return 0;
    int header=snprintf(tls->response,sizeof(tls->response),"HTTP/1.1 %s\r\nContent-Length: %d\r\n\r\n",
      wire_fault==2?"302 Found":"200 OK",bytes);
    memcpy(tls->response+header,payload,bytes);
    tls->size=header+bytes-((wire_fault==1 && bytes)?1:0);
  }
  if (n>29) n=29;
  if (n>tls->size-tls->pos) n=tls->size-tls->pos;
  memcpy(out,tls->response+tls->pos,n);
  tls->pos+=n;
  return n;
}
#include "community_https_under_test.inc"
void test_fetch(void) {
  if (task) {
    void (*fn)(void *)=task;
    task=NULL;
    fn(task_arg);
  }
}

extern bool model_row(size_t i,char *id,char *title,char *version,bool *owned,bool *persist);
bool pet_p4_miniapp_catalog_get(size_t i,pet_p4_miniapp_catalog_entry_t *e) {
  return model_row(i,e->widget_id,e->title,e->package_metadata.version,&e->community_owned,&e->community_persist);
}
extern void model_lane(bool begin);
void test_dispatch(bool admitted) {
  assert(queued);
  model_lane(true);
  pet_community_run(queued,admitted);
  model_lane(false);
  queued=NULL;
}
