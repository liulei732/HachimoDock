/* Run the production post-authentication handler against a counted state lane. */
#include "cJSON.h"
#include "../main/pet_p4_json_source.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>

#define LAN_MAINTENANCE_MODE 26
#define LAN_MAX_BODY 16384
#define LAN_IO_US 1000
#define ESP_MAC_BASE 0
#define ESP_OK 0
typedef unsigned mbedtls_gcm_context;
static atomic_bool usb_client_active;
static bool trusted=true,ota,reboot;
static uint32_t association=11;
static unsigned dispatched,streams;
static uint8_t first;
static uint32_t bundle_u32(const uint8_t *p) {
  return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static int esp_read_mac(uint8_t *dst,int base) {
  assert(base==ESP_MAC_BASE); memcpy(dst,(uint8_t[]){1,2,3,4,5,6},6); return ESP_OK;
}
static int64_t esp_timer_get_time(void) { return 1; }
static bool pet_p4_ota_active(void) { return ota; }
static bool pet_p4_diagnostics_reboot_pending(void) { return reboot; }
static uint32_t pet_device_wifi_association(void) { return association; }
static bool bundle_wire_admit(uint8_t *p,uint32_t gen) {
  return gen==41 && trusted && !memcmp(p,"0123456789abcdef",16);
}
static void bundle_wire_session_kind(int fd,mbedtls_gcm_context *g,uint8_t *c,uint8_t *p,
                                     uint32_t gen,uint32_t assoc,bool repair) {
  assert(fd==3 && g && c && p && gen==41 && assoc==11 && repair); streams++;
}
static bool send_sealed_response(int fd,mbedtls_gcm_context *g,const uint8_t *header,uint8_t *cipher,
                                 uint8_t *data,size_t size,int64_t deadline) {
  assert(fd==3 && g && cipher && header[0]==26 && size && deadline>0);first=*data;memset(data,0,size);return true;
}
static void control_deliver(int fd,uint8_t mode,uint8_t *body,size_t size,uint32_t gen,const cJSON *q,
                            cJSON **reply,char *unused) {
  assert(fd==3 && mode==26 && body[35]<5 && size>36 && gen==41 && !unused);
  assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(q,"requestId")));
  dispatched++;*reply=cJSON_Parse("{\"ok\":true}");
}
#include "../main/pet_p4_lan_maintenance.inc"

static uint8_t data[LAN_MAX_BODY],cipher[LAN_MAX_BODY+16],header[3]={26},gmem;
static size_t frame(uint8_t op,const char *json) {
  memcpy(data,"0123456789abcdefp4-010203040506",31);
  memset(data+31,0,3);data[34]=11;data[35]=op;
  memcpy(data+36,json,strlen(json));return 36+strlen(json);
}
static void dispatch(size_t size,unsigned gen,unsigned expected_requests,unsigned expected_streams) {
  unsigned old=dispatched,previous=streams;mbedtls_gcm_context g=gmem;
  lan_maintenance_handle(3,&g,header,cipher,data,size,gen,1);
  assert(dispatched==old+expected_requests && streams==previous+expected_streams);
}
int main(void) {
  const char *q="{\"requestId\":\"0123456789abcdef0123456789abcdef\"}";
  for(unsigned op=0;op<5;op++){dispatch(frame(op,q),41,1,0);assert(first=='{');}
  for(unsigned at=0;at<36;at+= (at==0?16:at==16?18:1)){
    size_t n=frame(0,q);data[at]^=0x80;dispatch(n,41,0,0);
  }
  dispatch(frame(0,q),40,0,0);trusted=false;dispatch(frame(5,""),41,0,0);trusted=true;
  usb_client_active=true;dispatch(frame(0,q),41,0,0);usb_client_active=false;
  ota=true;dispatch(frame(0,q),41,0,0);ota=false;reboot=true;dispatch(frame(5,""),41,0,0);reboot=false;
  dispatch(frame(5,""),41,0,1);assert(first==1);dispatch(frame(5,q),41,0,0);assert(!first);
  dispatch(frame(0,"{\"requestId\":\"bad\"}"),41,0,0);
  dispatch(frame(0,"{\"requestId\":\"0123456789abcdef0123456789abcdef\",\"extra\":1}"),41,0,0);
  size_t n=frame(0,q);data[n]='x';dispatch(n+1,41,0,0);
  n=frame(0,q);data[n-2]=0;dispatch(n,41,0,0);
  dispatch(35,41,0,0);dispatch(549,41,0,0);
}
