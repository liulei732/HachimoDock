/* Included after the boot harness; real scheduler, grant worker, and storage
 * lane, with persisted NVS bytes and an independently controlled WiFi clock. */
static void run_local_worker(void) {
  assert(pet_local_alarm_work());
  state_depth=1;
  assert(pet_storage_lane_enter(&lane));
  pet_local_alarm_run();
  assert(state_depth==1 && pet_storage_lane_leave(&lane));
  state_depth=0;
}
static void open_app(const char *id) {
  strcpy(g_runtime.widget_id,id);
  g_runtime.active=true;
  g_runtime.cext.events=true;
  g_runtime.cext.alarms=boot_config();
}
static void choose(const char *id,unsigned mode) {
  open_app(id);
  assert(pet_local_alarm_open() && pet_local_alarm_choose(mode));
  run_local_worker();
}
static void restart_device(void) {
  boot_reset();
  /* Include a valid catalog slot beyond the 32 parked alarm entries. */
  strcpy(g_catalog[1].widget_id,"tea-timer");
  strcpy(g_catalog[2].widget_id,"break-count");
  g_catalog[1].slot=PET_P4_MINIAPP_CATALOG_MAX-1;
  memset(local_records,0,sizeof local_records);
  memset(&local_edit,0,sizeof local_edit);
  memset(&local_job,0,sizeof local_job);
  local_restore=local_restore_retry=local_restore_after=local_retry_ms=0;local_work_update();
  network_clock=false;
  pet_local_alarm_boot();
}
static void restore_slots(void) {
  while(local_restore) {
    unsigned before=package_reads;
    run_local_worker();
    assert(package_reads<=before+1);
  }
}
static void legacy_catalog_coldboot(void) {
  boot_reset();
  g_catalog_count=28;
  for(unsigned i=0;i<g_catalog_count;i++) {
    snprintf(g_catalog[i].widget_id,sizeof g_catalog[i].widget_id,"installed-%u",i);
    g_catalog[i].resource_state=1;g_catalog[i].slot=i;g_catalog[i].widget_checksum=123;
  }
  g_catalog[27].slot=PET_P4_MINIAPP_CATALOG_MAX-1;
  namespace_missing=true;nvs_blob_reads=0;
  pet_local_alarm_boot();
  assert(!pet_local_alarm_work()&&!nvs_blob_reads&&!package_reads&&!disk_reads);
  namespace_missing=false;
  pet_local_alarm_boot();
  assert(!pet_local_alarm_work()&&nvs_blob_reads==28&&!package_reads&&!disk_reads);
  /* A legacy alarm-only record is readable in the extended format. */
  local_alarm_record_t legacy={.mode=PET_ALARM_COUNT,.checksum=123};
  strcpy(legacy.id,g_catalog[27].widget_id);
  unsigned slot=g_catalog[27].slot;
  sizes[slot]=offsetof(local_alarm_record_t,program);memcpy(blobs[slot],&legacy,sizes[slot]);
  pet_local_alarm_boot();
  assert(local_restore==(UINT64_C(1)<<slot)&&!package_reads&&!disk_reads);
  sizes[slot]=0;
}
static void local_lifecycle(void) {
  legacy_catalog_coldboot();
  assert(pet_alarm_button_hit(252,8) && pet_alarm_button_hit(427,63) &&
         !pet_alarm_button_hit(428,64) && !pet_alarm_button_hit(436,32));
  restart_device();
  choose("tea-timer",PET_ALARM_ALERT);
  assert(local_alarm_mode("tea-timer")==PET_ALARM_ALERT);
  assert(pet_local_alarm_open());open_app("break-count");
  assert(!pet_local_alarm_choose(PET_ALARM_ALERT)); /* a dialog cannot switch target */
  choose("break-count",PET_ALARM_COUNT);
  assert(commits==2 && local_alarm_mode("break-count")==PET_ALARM_COUNT);
  restart_device();
  restore_slots();
  assert(package_reads==2 && !g_ext_background_notify);
  ext_alarm_entry_t *alarm=ext_alarm_entry("tea-timer",false);
  assert(alarm && alarm->alarms.items[0].waiting_for_clock);
  miniapp_ext_process(0,false);
  assert(!local_job.pending && !g_ext_notice_pending);
  network_clock=true; /* a new SNTP sample after reconnect, without the PC */
  miniapp_ext_process(0,false);
  assert(local_job.pending && alarm->alarms.items[0].pending);
  fail_commit=true;run_local_worker();
  assert(alarm->alarms.items[0].pending && !g_ext_notice_pending && commits==2);
  fail_commit=false;mono=30001;
  run_local_worker();miniapp_ext_process(mono,false);
  assert(!alarm->alarms.items[0].pending && commits==3 && !dispatches);
  char title[64],message[PET_CEV_ALARM_TEXT_MAX];
  pet_p4_miniapp_alarm_policy(false);
  assert(pet_p4_miniapp_alarm_notification(title,message) && !strcmp(message,"wake"));
  assert(!pet_p4_miniapp_alarm_notification(title,message));

  restart_device();restore_slots();network_clock=true;mono=40000;
  miniapp_ext_process(mono,false); /* simulate stale saved pre-delivery deadline */
  assert(!local_job.pending && commits==3 && !g_ext_notice_pending);
  open_app("break-count");
  ext_alarm_entry_t *counter=ext_alarm_entry("break-count",false);
  assert(counter && counter->alarms.items[0].every_ms==60000);
  g_runtime.cext.alarms=counter->alarms;counter->used=false;
  pet_cev_alarm_t *periodic=&g_runtime.cext.alarms.items[0];
  mono=610000;miniapp_ext_process(mono,false);
  assert(local_job.pending && local_job.due==1700000660000ULL);
  mono+=300000;miniapp_ext_process(mono,false); /* audio can hold the worker for minutes */
  assert(local_job.due==1700000960000ULL);
  miniapp_ext_process(mono,true);assert(!dispatches && periodic->pending);
  run_local_worker();miniapp_ext_process(mono,true);
  assert(!dispatches && !periodic->pending && !g_ext_notice_pending);
  pet_local_alarm_view_t view;
  assert(pet_local_alarm_view(&view) && view.declared==1 && view.mode==PET_ALARM_COUNT &&
         view.completed==1 && view.clock_valid && !strcmp(view.alarms[0],"Stretch"));
  restart_device();restore_slots();network_clock=true;mono=910000;
  miniapp_ext_process(mono,false); /* same periodic occurrence after reconnect/reboot */
  assert(!local_job.pending && !g_ext_notice_pending);
  open_app("tea-timer");
  assert(pet_local_alarm_open() && pet_local_alarm_choose(PET_ALARM_OFF));
  open_app("break-count"); /* navigating away cannot discard confirmed revocation */
  g_catalog_sequence++;
  run_local_worker();pet_p4_miniapp_alarm_policy(true);
  assert(local_alarm_mode("tea-timer")==PET_ALARM_OFF);
  restart_device();restore_slots();
  assert(!ext_alarm_entry("tea-timer",false) && ext_alarm_entry("break-count",false));
  assert(grant(true));network_clock=true;miniapp_ext_process(80000,false);
  assert(!g_ext_notice_pending && !local_job.pending);
  choose("tea-timer",PET_ALARM_ALERT);
  miniapp_catalog_item_t replacement=g_catalog[1];replacement.widget_checksum++;
  fail_commit=true;
  assert(!local_alarm_retire(replacement.slot,"tea-timer",&replacement));
  assert(local_alarm_mode("tea-timer")==PET_ALARM_ALERT);
  fail_commit=false;
  assert(local_alarm_retire(replacement.slot,"tea-timer",&replacement));
  restart_device(); /* catalog write failed after retire, old version still denied */
  assert(local_alarm_mode("tea-timer")==PET_ALARM_OFF);
  assert(local_alarm_retire(replacement.slot,"tea-timer",NULL));
  restart_device();
  assert(!local_alarm_mode("tea-timer") && local_alarm_mode("break-count")==PET_ALARM_COUNT);
  sizes[g_catalog[2].slot]=1;restart_device(); /* corrupt grant never becomes permission */
  assert(local_alarm_mode("break-count")==PET_ALARM_OFF);
  memset(blobs,0,sizeof blobs);memset(sizes,0,sizeof sizes);restart_device();
  boot_reset();g_catalog_count=0;memset(&g_runtime,0,sizeof g_runtime);
}
