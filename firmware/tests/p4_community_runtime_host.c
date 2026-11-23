/* Compiled twice. The linker drops unreachable firmware and flash code. A
 * declared local action reaching a host/media peripheral fails the probe. */
#ifdef COMMUNITY_RUNTIME_ADAPTERS
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define UNEXPECTED(name) int name(void) { abort(); }
UNEXPECTED(hachi_module_host_profile)
UNEXPECTED(hachi_module_network_profile)
UNEXPECTED(pet_app_host_action)
UNEXPECTED(pet_app_host_confirmed_action)
UNEXPECTED(pet_app_host_context)
UNEXPECTED(pet_app_host_enabled)
UNEXPECTED(pet_app_host_observation_current)
UNEXPECTED(pet_app_host_observed_action)
UNEXPECTED(pet_p4_audio_instrument_strike)
UNEXPECTED(pet_p4_media_action)
UNEXPECTED(pet_p4_media_live_action)
UNEXPECTED(pet_p4_touch_ready)
UNEXPECTED(pet_p4_widget_data_paging)
bool pet_p4_wasm_trial_action(const char *action,bool *accepted) {
  (void)action; (void)accepted;
  return false;
}
int xPortCanYield(void) { return 1; }
int xPortInIsrContext(void) { return 0; }
int xSemaphoreGiveRecursive(void *owner) { return !!owner; }
int xSemaphoreTakeRecursive(void *owner,uint32_t ticks) { return owner && ticks; }
uint32_t esp_random(void) { return 1; }
int64_t esp_timer_get_time(void) { return 1234567; }
#else
#include <assert.h>
#include <stdlib.h>
#include "pet_p4_widget_trace.h"
/* Host Clang disallows crossing a cleanup-scoped trace variable in unrelated
 * device-only activation code. Tracing is disabled in this single-thread run. */
#undef PET_WIDGET_TRACE_SCOPE
#define PET_WIDGET_TRACE_SCOPE(name,stage) pet_widget_trace_scope_t name={0}
#define ESP_ERR_INVALID_SIZE 0x108
#define ESP_ERR_INVALID_RESPONSE 0x109
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_calloc(n,s,c) calloc(n,s)
#define heap_caps_malloc(n,c) malloc(n)
#define heap_caps_free(p) free(p)
#define portMAX_DELAY UINT32_MAX
typedef int StaticSemaphore_t;
#include "pet_p4_miniapp.c"

/* A temporary directory substitutes for flash. Save serialization and restore
 * validation run in firmware, with separate processes for each app launch. */
bool pet_app_store_update(const char *id,const char *json) {
  FILE *f=fopen(id,"wb");
  if (!f) return false;
  bool ok=fwrite(json,1,strlen(json),f)==strlen(json);
  return fclose(f)==0 && ok;
}
bool pet_app_store_read(const char *id,char out[PET_APP_SAVE_BYTES]) {
  FILE *f=fopen(id,"rb");
  if (!f) return false;
  size_t n=fread(out,1,PET_APP_SAVE_BYTES,f);
  assert(!ferror(f) && n<PET_APP_SAVE_BYTES);
  fclose(f);
  out[n]=0;
  return true;
}
bool pet_app_store_read_cached(const char *id,char out[PET_APP_SAVE_BYTES]) {
  return pet_app_store_read(id,out);
}

int main(int argc,char **argv) {
  if (argc!=4 && argc!=5) return 2;
  char error[160]={0};
  pet_p4_miniapp_sprite_pack_t *sprites=calloc(1,sizeof(*sprites));
  bool ok=sprites && parse_runtime(&g_runtime,argv[1],argv[2],argv[3],sprites,error,sizeof(error));
  free(sprites);
  if (!ok) { fprintf(stderr,"%s\n",error); return 1; }
  g_runtime_mutex=(void *)1;
  if (argc==5 && !strcmp(argv[4],"restore") && !restore_app_runtime(&g_runtime)) return 1;
  for (unsigned i=0;i<3;i++) {
    printf("%s|%s|%s|%u\n",g_runtime.states[g_runtime.current_state],
      g_runtime.view.metric_value,g_runtime.view.headline,g_runtime.save_version);
    if (i<2 && !pet_p4_miniapp_dispatch_action(g_runtime.buttons[0].action,1000+i*100)) return 1;
  }
  if (argc==5 && !strcmp(argv[4],"save") && !save_app_runtime()) return 1;
  return 0;
}
#endif
