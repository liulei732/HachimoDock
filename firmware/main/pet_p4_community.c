#include "pet_p4_community.h"
#include "pet_p4_community_config.h"
#include "pet_p4_device_wifi.h"
#include "pet_p4_miniapp.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { BROWSE, INSTALL, REMOVE } operation_t;
typedef struct {
  operation_t operation;
  unsigned generation;
  pet_community_item_t item;
  char expected[48], after[44];
  pet_community_catalog_t catalog;
  pet_community_files_t files;
  char *body;
  bool verified;
} community_job_t;

/* The settings and protocol paths share the model mutex. The network task owns
 * only its job until it transfers it to the protocol queue. */
static atomic_uint generation;
static community_job_t *pending;
static EXT_RAM_BSS_ATTR pet_community_catalog_t catalog;
static pet_community_view_t page;
static char expected[48], local_id[44];

static bool current(void *context) {
  const community_job_t *job=context;
  return atomic_load(&generation)==job->generation && (job->operation==REMOVE || pet_device_wifi_online());
}

static void remote_selection(void) {
  page.installed=false;
  page.more=catalog.next[0]!=0;
  page.count=catalog.count;
  page.install=page.remove=false;
  expected[0]=page.title[0]=page.version[0]=page.id[0]=0;
  if (page.index>=catalog.count) return;
  const pet_community_item_t *item=&catalog.items[page.index];
  strcpy(page.id,item->id);
  strcpy(page.title,item->name);
  strcpy(page.version,item->version);
  page.privileges=item->privileges;
  page.added_save=item->privileges!=0;
  page.install=true;
  for (size_t i=0,n=pet_p4_miniapp_catalog_count();i<n;i++) {
    pet_p4_miniapp_catalog_entry_t entry;
    if (!pet_p4_miniapp_catalog_get(i,&entry) || strcmp(entry.widget_id,item->id)) continue;
    strcpy(expected,entry.package_metadata.version);
    page.added_save=page.privileges && !entry.community_persist;
    page.remove=entry.community_owned;
    page.install=entry.community_owned && pet_community_newer(item->version,expected);
    page.status=entry.community_owned ? (page.install?"可更新":"已安装") : "同名组件由其他来源安装";
    return;
  }
}

static void local_selection(void) {
  page.installed=true;
  page.more=page.install=page.remove=false;
  page.count=page.privileges=0;
  expected[0]=page.title[0]=page.version[0]=local_id[0]=page.id[0]=0;
  for (size_t i=0,n=pet_p4_miniapp_catalog_count();i<n;i++) {
    pet_p4_miniapp_catalog_entry_t e;
    if (!pet_p4_miniapp_catalog_get(i,&e) || !e.community_owned || strlen(e.widget_id)>=sizeof(local_id)) continue;
    if (page.count++!=page.index) continue;
    strcpy(local_id,e.widget_id);
    strcpy(page.id,e.widget_id);
    strcpy(expected,e.package_metadata.version);
    snprintf(page.version,sizeof(page.version),"%.*s",32,expected);
    strcpy(page.title,e.title[0]?e.title:e.widget_id);
    page.remove=true;
  }
  page.status=page.count?"已安装的社区组件":"没有已安装的社区组件";
}

void pet_community_local(void) {
  if (pending) return;
  page.index=0;
  local_selection();
}

void pet_community_view(pet_community_view_t *out) {
  *out=page;
  out->busy=pending!=NULL;
}
void pet_community_close(void) {
  atomic_fetch_add(&generation,1);
  page=(pet_community_view_t){0};
  catalog.count=0;
  catalog.next[0]=0;
}

static void network_task(void *arg) {
  community_job_t *job=arg;
  size_t size=0;
  const size_t limit=job->operation==BROWSE?8192:job->item.bytes;
  char path[256], nonce[33], digest[65];
  if (job->operation==BROWSE) {
    uint8_t random[16];
    esp_fill_random(random,sizeof(random));
    for (unsigned i=0;i<16;i++) snprintf(nonce+2*i,3,"%02x",random[i]);
    snprintf(path,sizeof(path),"/api/v1/device/catalog?board=wifi6-p4&profile=p4-local-v1&nonce=%s&after=%s",nonce,job->after);
  } else {
    snprintf(path,sizeof(path),"/api/v1/device/payload/wifi6-p4/%s/%s/%s",
      job->item.id,job->item.version,job->item.digest);
  }
  job->body=heap_caps_malloc(limit+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if (job->body && pet_community_https(path,job->body,limit,&size,current,job)) {
    const uint8_t *raw=(const uint8_t *)job->body;
    job->verified=job->operation==BROWSE ?
      pet_community_verify_catalog(raw,size,PET_COMMUNITY_KEY_B64,nonce,job->after,&job->catalog) :
      (size==job->item.bytes && pet_community_hash(raw,size,digest) && !strcmp(digest,job->item.digest) &&
        pet_community_parse_payload(&job->item,raw,size,&job->files));
  }
  /* Bind local ownership to the pinned signer. A later firmware pointed at a
   * different catalog cannot inherit an existing ID or its retained schema. */
  if (job->verified && job->operation==INSTALL) {
    job->verified=pet_community_hash(PET_COMMUNITY_KEY_B64,strlen(PET_COMMUNITY_KEY_B64),digest);
    if (job->verified) {
      if (job->item.privileges) {
        memmove(job->files.save_contract+32,job->files.save_contract,32);
        job->files.save_contract[64]=0;
      } else job->files.save_contract[32]=0;
      memcpy(job->files.save_contract,digest,32);
    }
  }
  /* A full protocol queue is transient. The job stays owned until handed off. */
  while (!pet_community_schedule(job)) vTaskDelay(pdMS_TO_TICKS(30)+1);
  vTaskDelete(NULL);
}

static void start(operation_t op,bool next) {
  if (pending) return;
  if (op!=REMOVE && !PET_COMMUNITY_KEY_B64[0]) { page.status="未配置社区签名密钥"; return; }
  if (op!=REMOVE && !pet_device_wifi_online()) { page.status="请先连接 WIFI"; return; }
  community_job_t *job=heap_caps_calloc(1,sizeof(*job),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if (!job) { page.status="内存不足"; return; }
  job->generation=atomic_load(&generation);
  job->operation=op;
  if (op==BROWSE) {
    if (next) strcpy(job->after,catalog.next);
    /* A failed refresh must never leave actionable rows from an older query. */
    catalog.count=0; catalog.next[0]=0;
    page=(pet_community_view_t){0};
  } else {
    if (page.installed) strcpy(job->item.id,local_id);
    else job->item=catalog.items[page.index];
    strcpy(job->expected,expected);
  }
  pending=job;
  page.status="正在处理";
  bool queued=op==REMOVE ? pet_community_schedule(job) :
    xTaskCreate(network_task,"community",10240,job,3,NULL)==pdPASS;
  if (!queued) { pending=NULL; free(job); page.status="设备忙，请重试"; }
}

void pet_community_browse(bool next) {
  if (pending || (next && !catalog.next[0])) return;
  page.installed=false;
  if (!next) { page=(pet_community_view_t){0}; catalog.count=0; catalog.next[0]=0; }
  start(BROWSE,next);
}
void pet_community_move(int delta) {
  if (pending || !page.count) return;
  if (delta>0 && page.index+1==page.count && page.more) { pet_community_browse(true); return; }
  page.index=delta<0 ? (page.index?page.index-1:page.count-1) : (page.index+1)%page.count;
  page.status=NULL;
  if (page.installed) local_selection(); else remote_selection();
}
void pet_community_action(bool remove) {
  if (page.index>=page.count || !(remove?page.remove:page.install)) return;
  start(remove?REMOVE:INSTALL,false);
}

void pet_community_run(void *context,bool admitted) {
  community_job_t *job=context;
  if (!job || job!=pending) return;
  if (job->generation==atomic_load(&generation)) {
    if (job->operation==BROWSE) {
      if (job->verified && current(job)) {
        catalog=job->catalog;
        page.index=0;
        page.status=catalog.count?NULL:"暂无兼容组件";
        remote_selection();
      } else page.status="目录验证或连接失败";
    } else {
      bool ok=admitted && current(job) && (job->operation==REMOVE || job->verified) &&
        pet_community_store(&job->item,job->expected,job->operation==REMOVE?NULL:&job->files,current,job);
      if (job->generation==atomic_load(&generation)) {
        if (page.installed) { page.index=0; local_selection(); }
        else remote_selection();
        page.status=ok?(job->operation==REMOVE?"已删除，保留存档":"已安装"):"操作失败，请刷新后重试";
      }
    }
  }
  pending=NULL;
  free(job->body); free(job);
}
