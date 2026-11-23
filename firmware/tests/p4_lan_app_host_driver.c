/* Runs in the real app host test TU, at the authenticated record boundary. */
#include <stdatomic.h>
#define LAN_WIDGET_BOARD_BYTES 15
#define LAN_WIDGET_PREFIX_BYTES 31
#define pdTRUE 1
static int lock = 1;
static uint64_t now = 100, client_seen_ms;
static atomic_bool control_trusted = true, usb_client_active;
static uint32_t pairing_generation = 1;
static bool volume_available(void) { return true; }
static int xSemaphoreTake(int mutex, int timeout) { (void)timeout; return mutex; }
static void xSemaphoreGive(int mutex) { (void)mutex; }
#define portMAX_DELAY 100
static uint64_t clock_ms(void) { return now; }
#define generation pairing_generation
#include "pet_p4_lan_app_host.inc"
#undef generation

static const char board[] = "p4-001122334455";
static uint8_t frame[LAN_APP_PREFIX],reply[LAN_APP_HEADER+LAN_APP_JSON_MAX+1];
static cJSON *poll_event(void) {
  memset(frame+47,0,5);
  assert(app_exchange(frame,NULL,reply,board,++now));
  assert(reply[0]==3 && reply[4]);
  memcpy(frame+47,reply+1,4);
  return reply[5] ? cJSON_Parse((char *)reply+5) : NULL;
}
static bool deliver(uint8_t operation,cJSON *payload) {
  frame[51]=operation;
  reply[0]=0;
  return app_exchange(frame,payload,reply,board,++now);
}
static void usb_sink(const char *line,void *ctx) { (void)line; (void)ctx; }
int main(void) {
  memcpy(frame,"0123456789abcdefp4-001122334455",31);
  frame[31]=42;
  const char *binding="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  assert(app_status()==1 && app_widget_ack(1,true)==1);
  pet_app_host_tick_bound("bridge-app",1,0,true,now,binding);
  assert(app_status()==3 && !app_channel.event);
  assert(app_widget_ack(0,true)==0 && app_widget_ack(1,false)==1 && app_widget_ack(1,true)==3);
  assert(!app_channel.token && !app_channel.event);
  assert(!poll_event()); /* first authenticated poll installs the emitter */
  pet_app_host_tick_bound("bridge-app",1,0,true,++now,binding);
  cJSON *enter=poll_event(); assert(enter && deliver(APP_VALIDATE,enter));
  cJSON *snap=cJSON_Parse("{\"componentId\":\"bridge-app\",\"sequence\":1,\"values\":{},\"ui\":{\"mode\":0,\"actions\":[\"host.next\"],\"context\":\"abc123\"},\"presentation\":{\"images\":{\"cover\":{\"key\":\"asset-a\",\"revision\":\"one\",\"width\":8,\"height\":8}}}}");
  cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(snap,"presentation"),"timelines");
  cJSON_AddStringToObject(snap,"contract",binding);
  cJSON_AddStringToObject(snap,"instance",str(enter,"instance"));
  assert(deliver(APP_SNAPSHOT,snap)); assert(!deliver(APP_SNAPSHOT,snap));
  cJSON *im=cJSON_Parse("{\"componentId\":\"bridge-app\",\"context\":\"abc123\",\"alias\":\"cover\",\"key\":\"asset-a\",\"revision\":\"one\",\"width\":8,\"height\":8,\"offset\":0,\"data\":\"fixture\"}");
  cJSON_AddStringToObject(im,"instance",str(enter,"instance"));
  assert(deliver(APP_IMAGE,im));
  cJSON_AddStringToObject(im,"operation","query");
  assert(deliver(APP_IMAGE,im) && reply[0]==2);
  cJSON_DeleteItemFromObjectCaseSensitive(im,"operation");

  assert(pet_app_host_confirmed_action("bridge-app",1,0,"host.next","abc123",++now));
  now+=2100;
  pet_app_host_tick_bound("bridge-app",1,0,true,now,binding);
  cJSON *action=poll_event();
  assert(action && !strcmp(str(action,"action"),"host.next"));
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(action,"confirmed")));
  assert(!strcmp(str(action,"context"),"abc123") && deliver(APP_VALIDATE,action));
  assert(!poll_event()); /* lost action receipt or poll response cannot replay */
  pet_app_host_tick(NULL,0,0,false,++now);
  assert(!deliver(APP_VALIDATE,action) && !deliver(APP_IMAGE,im));
  assert(app_status()==3); /* pending exit survives idle transition */
  cJSON *exit=poll_event(); assert(exit && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(exit,"visible")));
  assert(app_status()==1 && !deliver(APP_SNAPSHOT,snap));

  pet_app_host_tick_bound("bridge-app",1,0,true,++now,binding);
  cJSON *next=poll_event();
  assert(next && strcmp(str(next,"instance"),str(enter,"instance")));
  assert(!deliver(APP_VALIDATE,action));
  frame[31]++; assert(!deliver(APP_VALIDATE,next));
  frame[50]=0; assert(!deliver(APP_POLL,NULL)); /* another process cannot steal a live lease */
  frame[50]=(uint8_t)app_channel.token; frame[31]--;
  frame[16]='X'; assert(!deliver(APP_VALIDATE,next)); frame[16]='p';
  frame[50]++; assert(!deliver(APP_VALIDATE,next)); frame[50]--;
  pairing_generation++; assert(!deliver(APP_VALIDATE,next)); assert(!poll_event());
  assert(!deliver(APP_SNAPSHOT,snap));
  pet_app_host_transport(usb_sink,NULL,board);
  pet_app_host_tick_bound("bridge-app",1,0,true,++now,binding);
  assert(!deliver(APP_VALIDATE,next) && !deliver(APP_IMAGE,im));
  cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(snap,"instance"),instance);
  assert(!deliver(APP_SNAPSHOT,snap)); /* even a matching new USB view is outside this route */
  now+=LAN_APP_LEASE_MS;
  assert(!deliver(APP_VALIDATE,next));
  app_retire();
  cJSON_Delete(next); cJSON_Delete(exit); cJSON_Delete(action);
  cJSON_Delete(im); cJSON_Delete(snap); cJSON_Delete(enter);
  return 0;
}
