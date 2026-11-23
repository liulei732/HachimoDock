/* Exercise the actual post-AEAD dispatcher with deterministic session and backend state. */
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned generation=41, association=3, writes, queries;
static bool paired=true, online=true, trusted=true, forgetting=false, wired, ota, rebooting, locked;
#define usb_client_active wired
#define control_trusted trusted
#define atomic_load(p) (*(p))
#define PET_NET_DENIED (-2)
#define xSemaphoreTake(l,t) do { assert(!locked); locked=true; } while(0)
#define xSemaphoreGive(l) do { assert(locked); locked=false; } while(0)
static bool volume_available(void) { return paired && online && !forgetting; }
static const char *pet_p4_boot_identity(void) { return "aabbccdd11223344"; }
static unsigned pet_device_wifi_association(void) { return association; }
static bool pet_p4_ota_active(void) { return ota; }
static bool pet_p4_diagnostics_reboot_pending(void) { return rebooting; }
static uint32_t bundle_u32(const uint8_t *b) {
  return (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3];
}
static void mbedtls_platform_zeroize(void *p,size_t n) { memset(p,0,n); }
static cJSON *pet_net_device_status(void) {
  assert(locked); queries++; return cJSON_Parse("{\"ok\":true,\"active\":true}");
}
static int pet_net_device_provision_target(const char *owner,const char *name,const char *value,const char *target) {
  assert(locked && !strcmp(owner,"test-app") && !strcmp(name,"session") && *value);
  assert(!target || !strcmp(target,"https://api.example.org")); writes++; return 79;
}
#include "../main/pet_p4_lan_component_network.inc"
static uint8_t buffer[LAN_NET_MAX+2];
static size_t frame(unsigned op) {
  memset(buffer,0,sizeof buffer); memcpy(buffer,pet_p4_boot_identity(),16);
  buffer[19]=3; buffer[20]=op;
  if(op!=LAN_NET_SET) return LAN_NET_PREFIX;
  const char data[]="test-app\0session\0https://api.example.org\0SYNTHETIC-SENSITIVE-MARKER";
  memcpy(buffer+LAN_NET_PREFIX,data,sizeof data);
  return LAN_NET_PREFIX+sizeof data;
}
static void check(size_t n,unsigned gen,bool accepted) {
  size_t len=lan_component_net_request(buffer,n,gen);
  assert(len<LAN_NET_REPLY && len==strlen((char*)buffer));
  cJSON *r=cJSON_Parse((char*)buffer);
  assert(r && cJSON_IsTrue(cJSON_GetObjectItem(r,"ok"))==accepted);
  assert(!strstr((char*)buffer,"session") && !strstr((char*)buffer,"MARKER"));
  for(size_t i=len+1;i<n;i++) assert(buffer[i]==0);
  cJSON_Delete(r);
}
int main(void) {
  check(frame(LAN_NET_STATUS),generation,true); check(frame(LAN_NET_SET),generation,true);
  size_t n=frame(LAN_NET_SET);
  buffer[n++]=1; check(n,generation,false);
  n=frame(LAN_NET_SET); check(n-1,generation,false);
  n=frame(LAN_NET_STATUS); check(n+1,generation,false);
  check(frame(3),generation,false);
  for(unsigned i=0;i<21;i++){frame(LAN_NET_SET);check(i,generation,false);}
  frame(LAN_NET_SET);check(LAN_NET_MAX+1,generation,false);
  for(unsigned op=1;op<=2;op++){
    n=frame(op);buffer[0]^=1;check(n,generation,false);
    n=frame(op);buffer[19]++;check(n,generation,false);
    check(frame(op),generation+1,false);
    bool *off[]={&paired,&trusted,&online};
    for(unsigned j=0;j<3;j++){*off[j]=false;check(frame(op),generation,false);*off[j]=true;}
    forgetting=true;check(frame(op),generation,false);forgetting=false;
  }
  bool *block[]={&wired,&ota,&rebooting};
  for(unsigned i=0;i<3;i++){*block[i]=true;check(frame(LAN_NET_SET),generation,false);*block[i]=false;}
  assert(writes==1 && queries==1 && !locked);
}
