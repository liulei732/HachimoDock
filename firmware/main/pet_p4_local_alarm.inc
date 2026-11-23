/* Device-owned decisions and the last consumed UTC occurrence per declaration.
 * One NVS blob per physical catalog slot. Only the storage worker (or single
 * threaded boot) changes flash. Input/periodic processing only reserve work. */
typedef struct {
  char id[PET_CEV_WIDGET_ID_MAX];
  uint32_t checksum, completed, mode;
  uint64_t delivered[PET_CEV_ALARMS];
  uint32_t program, buttons, sprites; /* Optional extension to the original alarm blob. */
} local_alarm_record_t;

static EXT_RAM_BSS_ATTR local_alarm_record_t local_records[PET_P4_MINIAPP_CATALOG_MAX];
static struct {
  char title[64], text[PET_CEV_ALARM_TEXT_MAX], alarm[PET_CEV_NAME_MAX];
  uint64_t due;
  unsigned slot, index;
  uint32_t serial;
  bool pending;
} local_job;
static struct {
  char id[PET_CEV_WIDGET_ID_MAX];
  uint32_t sequence, checksum;
  unsigned slot, mode;
  uint64_t retry_after;
  bool failed;
} local_edit;
static atomic_bool local_work;
_Static_assert(PET_P4_MINIAPP_CATALOG_MAX <= 64, "catalog restore bitmap");
static uint64_t local_restore, local_restore_retry, local_restore_after;
static uint64_t local_retry_ms;
#if PET_COMPONENT_BACKGROUND_ENABLED
static uint64_t program_pending, program_retry, program_retry_after;
static bool program_loading;
#endif

static void local_work_update(void) {
  bool work = local_job.pending || local_edit.mode || local_restore || local_restore_retry;
#if PET_COMPONENT_BACKGROUND_ENABLED
  work |= program_pending != 0 || program_retry != 0;
#endif
  atomic_store(&local_work,work);
}
static void local_alarm_cancel_job(const char *id, const char *alarm) {
  if (local_job.pending && !strcmp(local_records[local_job.slot].id,id) &&
      (!alarm || !strcmp(local_job.alarm,alarm))) {
    local_job.pending = false;
    local_work_update();
  }
}
static int local_alarm_find(const char *id) {
  for (unsigned i = 0; i < PET_P4_MINIAPP_CATALOG_MAX; i++)
    if (local_records[i].mode && !strcmp(local_records[i].id,id)) return (int)i;
  return -1;
}
static unsigned local_alarm_mode(const char *id) {
  int slot = local_alarm_find(id);
  return slot < 0 ? 0 : local_records[slot].mode;
}
static uint64_t local_alarm_due(const pet_cev_alarm_t *a) {
  return a->armed && a->every_ms && a->utc_deadline_ms >= a->every_ms
    ? a->utc_deadline_ms - a->every_ms : a->utc_deadline_ms;
}
/* For a foreground transition, retain an in-flight hidden occurrence until
 * its claim completes. A previously committed occurrence has no second effect. */
static bool local_alarm_foreground(const char *id,unsigned index,pet_cev_alarm_t *a) {
  if (!a->background || !a->utc_deadline) return true;
  int slot = local_alarm_find(id);
  if (slot < 0) return true;
  uint64_t due = local_alarm_due(a);
  if (due <= local_records[slot].delivered[index]) { a->pending = false; return false; }
  return !(local_job.pending && local_job.slot == (unsigned)slot &&
           local_job.index == index && local_job.due == due);
}
static bool local_alarm_offer(const char *id, const char *title, unsigned index, const pet_cev_alarm_t *a) {
  int slot = local_alarm_find(id);
  if (slot < 0 || index >= PET_CEV_ALARMS) return false;
  const local_alarm_record_t *r = &local_records[slot];
  if (r->mode == PET_ALARM_OFF) return true;
  if (!a->utc_deadline) return false;
  const uint64_t due = local_alarm_due(a);
  if (due <= r->delivered[index]) return true;
  if (local_job.pending) {
    /* Audio/storage contention may span several periods. Keep one latest
     * occurrence even when the worker has already reserved this alarm. */
    if (local_job.slot == (unsigned)slot && local_job.index == index && due > local_job.due) {
      local_job.due = due;
      ++local_job.serial;
    }
    return false;
  }
  if (local_edit.mode || (r->mode == PET_ALARM_ALERT && g_ext_notice_pending)) return false;
  local_job.slot = (unsigned)slot;
  local_job.index = index;
  local_job.due = due;
  ++local_job.serial;
  copy_utf8(local_job.title,sizeof(local_job.title),title);
  copy_utf8(local_job.text,sizeof(local_job.text),a->text);
  copy_utf8(local_job.alarm,sizeof(local_job.alarm),a->id);
  local_job.pending = true;
  local_work_update();
  return false; /* Keep the occurrence pending until flash acknowledges it. */
}

static void local_alarm_key(char key[12], unsigned slot) {
  snprintf(key,12,"alarm%u",slot);
}
static bool local_alarm_write(unsigned slot, const local_alarm_record_t *r) {
  nvs_handle_t h;
  if (nvs_open("p4_alarms",NVS_READWRITE,&h) != ESP_OK) return false;
  char key[12]; local_alarm_key(key,slot);
  bool ok = nvs_set_blob(h,key,r,sizeof(*r)) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}
/* Before a real replace/delete, including preserve-save deletion. If flash
 * cannot confirm retirement the catalog transaction may not proceed. */
static bool local_alarm_retire(unsigned slot, const char *id, const miniapp_catalog_item_t *replacement) {
  local_alarm_record_t retired = {0};
#if PET_COMPONENT_BACKGROUND_ENABLED
  /* A same-content reinstall can retain a live registry entry. Local resume
   * ownership must end even in that case. This runs on the storage lane. */
  ext_lock();
  bool program = local_records[slot].program == PET_LOCAL_PROGRAM_ON;
  ext_unlock();
  if (program && !background_prepare_remove(id)) return false;
#endif
  nvs_handle_t h;
  esp_err_t err = nvs_open("p4_alarms",NVS_READONLY,&h);
  char key[12]; local_alarm_key(key,slot);
  if (err == ESP_OK) {
    size_t n = 0;
    err = nvs_get_blob(h,key,NULL,&n);
    nvs_close(h);
    if (err == ESP_OK) {
      if (replacement) {
        copy_utf8(retired.id,sizeof(retired.id),id);
        ext_lock();
        retired.mode = local_records[slot].mode ? PET_ALARM_OFF : 0;
        ext_unlock();
        retired.checksum = replacement->widget_checksum;
        if (!local_alarm_write(slot,&retired)) return false;
      } else {
        if (nvs_open("p4_alarms",NVS_READWRITE,&h) != ESP_OK) return false;
        err = nvs_erase_key(h,key);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
      }
    }
  }
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
  ext_lock();
  local_alarm_cancel_job(id,NULL);
  local_records[slot] = retired;
  local_restore &= ~(UINT64_C(1)<<slot);
  local_restore_retry &= ~(UINT64_C(1)<<slot);
#if PET_COMPONENT_BACKGROUND_ENABLED
  program_pending &= ~(UINT64_C(1)<<slot);
  program_retry &= ~(UINT64_C(1)<<slot);
#endif
  local_work_update();
  if (local_edit.mode && !strcmp(local_edit.id,id)) {
    local_edit.mode = 0; local_edit.failed = true; local_work_update();
  }
  if (!strcmp(g_ext_notice_id,id)) g_ext_notice_pending = false;
  ext_unlock();
  return true;
}

void pet_local_alarm_boot(void) {
  nvs_handle_t h;
  esp_err_t opened = nvs_open("p4_alarms",NVS_READONLY,&h);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return;
  for (unsigned i = 0; i < g_catalog_count; ++i) {
    const miniapp_catalog_item_t *item = &g_catalog[i];
    char key[12]; local_alarm_key(key,item->slot);
    local_alarm_record_t r = {0};
    size_t n = sizeof(r);
    if (item->resource_state != 1) continue;
    esp_err_t err = opened == ESP_OK ? nvs_get_blob(h,key,&r,&n) : opened;
    if (err == ESP_ERR_NVS_NOT_FOUND) continue;
    if (err != ESP_OK || (n != sizeof(r) && n != offsetof(local_alarm_record_t,program)) || !memchr(r.id,0,sizeof(r.id)) ||
        strcmp(r.id,item->widget_id) || r.checksum != item->widget_checksum ||
        r.mode > PET_ALARM_ALERT) {
      r = (local_alarm_record_t){.mode=PET_ALARM_OFF,.checksum=item->widget_checksum};
      copy_utf8(r.id,sizeof(r.id),item->widget_id);
    }
#if PET_COMPONENT_BACKGROUND_ENABLED
    if (r.program == PET_LOCAL_PROGRAM_ON && r.buttons == item->buttons_checksum &&
        r.sprites == item->sprites_checksum && n == sizeof(r))
      program_pending |= UINT64_C(1)<<item->slot;
    else r.program = 0;
#endif
    local_records[item->slot] = r;
    if (r.mode >= PET_ALARM_COUNT) local_restore |= UINT64_C(1) << item->slot;
  }
  if (opened == ESP_OK) nvs_close(h);
  ext_lock(); local_work_update(); ext_unlock();
}

static int local_alarm_active(void) {
  if (!g_runtime.active || g_catalog_recovery_blocked) return -1;
#if PET_COMPONENT_BACKGROUND_ENABLED
  if (g_runtime.program.local_background) return catalog_find(g_catalog,g_catalog_count,g_runtime.widget_id);
#endif
  for (unsigned i = 0; i < g_runtime.cext.alarms.count; ++i)
    if (g_runtime.cext.alarms.items[i].background) return catalog_find(g_catalog,g_catalog_count,g_runtime.widget_id);
  return -1;
}
bool pet_local_alarm_view(pet_local_alarm_view_t *out) {
  miniapp_runtime_lock();
  int i = local_alarm_active();
  bool ok = i >= 0 && g_catalog[i].resource_state == 1;
  if (ok && out) {
    *out = (pet_local_alarm_view_t){0};
    copy_utf8(out->title,sizeof(out->title),g_runtime.view.title);
    copy_utf8(out->id,sizeof(out->id),g_runtime.widget_id);
    for (unsigned j = 0; j < g_runtime.cext.alarms.count; ++j) {
      const pet_cev_alarm_t *a = &g_runtime.cext.alarms.items[j];
      if (a->background && out->declared < PET_CEV_ALARMS) {
        copy_utf8(out->alarms[out->declared], sizeof(out->alarms[0]), a->text);
        out->state[out->declared] = a->cancelled ? 3 : a->pending ? 2 : a->armed ? 1 : 0;
        ++out->declared;
      }
    }
    ext_lock();
    local_alarm_record_t *r = &local_records[g_catalog[i].slot];
    out->mode = r->mode; out->completed = r->completed;
#if PET_COMPONENT_BACKGROUND_ENABLED
    out->program = g_runtime.program.local_background;
    out->program_allowed = r->program == PET_LOCAL_PROGRAM_ON;
    out->program_waiting = ((program_pending|program_retry) & (UINT64_C(1)<<g_catalog[i].slot)) != 0;
#endif
    if (!strcmp(local_edit.id,g_runtime.widget_id)) {
      out->pending = local_edit.mode != 0;
      out->failed = local_edit.failed;
    }
    ext_unlock();
#if PET_COMPONENT_BACKGROUND_ENABLED
    if (out->program_allowed && g_background_mutex) {
      pet_program_registration_t reg={0};
      xSemaphoreTake(g_background_mutex,portMAX_DELAY);
      out->program_running = background_matches_under_registry(&g_runtime,&reg) &&
          g_background_programs.entries[reg.slot].session.live && !g_background_programs.entries[reg.slot].stopped;
      xSemaphoreGive(g_background_mutex);
    }
#endif
    out->clock_valid = miniapp_utc_snapshot(esp_timer_get_time()/1000ULL).valid;
  }
  miniapp_runtime_unlock(); return ok;
}
bool pet_local_alarm_open(void) {
  if (pet_p4_miniapp_storage_busy()) return false;
  miniapp_runtime_lock(); ext_lock();
  int i = local_alarm_active();
  bool ok = !local_edit.mode && i >= 0 && g_catalog[i].resource_state == 1;
  if (ok) {
    copy_utf8(local_edit.id,sizeof(local_edit.id),g_runtime.widget_id);
    local_edit.sequence = g_catalog_sequence;
    local_edit.slot = g_catalog[i].slot;
    local_edit.checksum = g_catalog[i].widget_checksum;
    local_edit.failed = false;
  }
  ext_unlock(); miniapp_runtime_unlock(); return ok;
}
bool pet_local_alarm_choose(unsigned mode) {
  bool program = mode == PET_LOCAL_PROGRAM_ON || mode == PET_LOCAL_PROGRAM_OFF;
  if (mode < PET_ALARM_OFF || mode > PET_LOCAL_PROGRAM_OFF) return false;
  bool busy = pet_p4_miniapp_storage_busy();
  miniapp_runtime_lock(); ext_lock();
#if PET_COMPONENT_BACKGROUND_ENABLED
  if (mode == PET_LOCAL_PROGRAM_OFF && program_loading) busy = false;
#endif
  bool ok = !busy && !local_edit.mode && local_edit.sequence == g_catalog_sequence &&
    local_alarm_active() >= 0 && !strcmp(local_edit.id,g_runtime.widget_id);
#if PET_COMPONENT_BACKGROUND_ENABLED
  if (ok && program) ok = g_runtime.program.local_background && g_background_ready;
#else
  if (program) ok = false;
#endif
  if (ok && !program) {
    ok = false;
    for (unsigned j=0;j<g_runtime.cext.alarms.count;j++) ok |= g_runtime.cext.alarms.items[j].background;
  }
  if (ok) {
    local_edit.mode = mode; local_edit.failed = false; local_edit.retry_after = 0;
    if (mode == PET_ALARM_OFF) {
      local_alarm_record_t *r = &local_records[local_edit.slot];
      copy_utf8(r->id,sizeof(r->id),local_edit.id);
      r->checksum = local_edit.checksum;
      r->mode = PET_ALARM_OFF; /* A failed save still stops this boot. */
    }
    if (!program) {
      local_alarm_cancel_job(local_edit.id,NULL);
      if (!strcmp(g_ext_notice_id,local_edit.id)) g_ext_notice_pending = false;
    }
#if PET_COMPONENT_BACKGROUND_ENABLED
    if (mode == PET_LOCAL_PROGRAM_OFF) {
      local_records[local_edit.slot].program = 0;
      uint64_t bit = UINT64_C(1)<<local_edit.slot;
      program_pending &= ~bit;
      program_retry &= ~bit;
      /* Stop execution before returning the physical input event. Persistence
       * and release/checkpoint run on the worker. Model -> registry order. */
      pet_program_registration_t reg={0};
      xSemaphoreTake(g_background_mutex,portMAX_DELAY);
      if (background_matches_under_registry(&g_runtime,&reg))
        (void)pet_program_registry_stop(&g_background_programs,reg);
      xSemaphoreGive(g_background_mutex);
    }
#endif
    local_work_update();
  } else if (!local_edit.mode) {
    local_edit.failed = true;
  }
  ext_unlock(); miniapp_runtime_unlock(); return ok;
}

bool pet_local_program_toggle(void) {
#if PET_COMPONENT_BACKGROUND_ENABLED
  ext_lock();
  bool stop = local_edit.id[0] && local_records[local_edit.slot].program == PET_LOCAL_PROGRAM_ON;
  ext_unlock();
  return pet_local_alarm_choose(stop ? PET_LOCAL_PROGRAM_OFF : PET_LOCAL_PROGRAM_ON);
#else
  return false;
#endif
}

#if PET_COMPONENT_BACKGROUND_ENABLED
/* Caller owns the protocol command lane and holds the state mutex. The
 * existing verified loader releases it during IO, then fences publication. */
static bool local_program_authorized(unsigned slot,const char *id,const uint32_t content[3]) {
  ext_lock();
  const local_alarm_record_t *r=&local_records[slot];
  bool allowed = r->program == PET_LOCAL_PROGRAM_ON && !strcmp(r->id,id) &&
                 r->checksum == content[0] && r->buttons == content[1] && r->sprites == content[2];
  ext_unlock();
  return allowed && miniapp_utc_snapshot(esp_timer_get_time()/1000ULL).valid;
}
static void local_program_resume(unsigned slot,uint64_t now) {
  uint64_t bit = UINT64_C(1)<<slot;
  ext_lock();
  local_alarm_record_t record = local_records[slot];
  program_loading = true;
  ext_unlock();
  uint32_t content[3] = {record.checksum,record.buttons,record.sprites};
  uint64_t registration = 0;
  char error[128] = {0};
  bool ok = record.program == PET_LOCAL_PROGRAM_ON &&
      background_start_installed(record.id,content,&registration,error,sizeof(error),true,NULL,NULL);
  ext_lock();
  program_loading = false;
  program_pending &= ~bit;
  if (!ok && local_records[slot].program == PET_LOCAL_PROGRAM_ON) {
    program_retry |= bit;
    program_retry_after = now + 30000;
  }
  local_work_update();
  ext_unlock();
}
#endif
static bool alarm_restore_saved(int local_slot);
bool pet_local_alarm_work(void) { return atomic_load(&local_work); }
void pet_local_alarm_run(void) {
  uint64_t now = esp_timer_get_time()/1000ULL;
  ext_lock();
  if (local_edit.mode && now < local_edit.retry_after) { ext_unlock(); return; }
  if (local_restore_retry && now >= local_restore_after) {
    local_restore |= local_restore_retry;
    local_restore_retry = 0;
  }
  bool deliver = local_job.pending && now >= local_retry_ms;
  if (!local_edit.mode && !deliver) {
    uint64_t pending = local_restore;
#if PET_COMPONENT_BACKGROUND_ENABLED
    if (program_retry && now >= program_retry_after) {
      program_pending |= program_retry;
      program_retry = 0;
    }
    uint64_t programs = program_pending;
#endif
    ext_unlock();
#if PET_COMPONENT_BACKGROUND_ENABLED
    if (!pending && programs && miniapp_utc_snapshot(now).valid) {
      local_program_resume((unsigned)__builtin_ctzll(programs),now);
      return;
    }
#endif
    if (pending) {
      unsigned slot = (unsigned)__builtin_ctzll(pending);
      uint64_t bit = UINT64_C(1) << slot;
      bool ok = alarm_restore_saved((int)slot);
      ext_lock();
      local_restore &= ~bit;
      if (!ok) {
        local_restore_retry |= bit;
        local_restore_after = now + 30000;
      }
      local_work_update();
      ext_unlock();
    }
    return;
  }
  ext_unlock();
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish))) = {0};
  if (!storage_scope_begin(&storage,false) || !storage.may_unlock ||
      !pet_storage_scope_advance(&storage,PET_STORAGE_PREPARE)) return;
  miniapp_runtime_lock();
  ext_lock();
  unsigned mode = local_edit.mode, slot = local_job.slot;
  uint32_t serial = local_job.serial;
  local_alarm_record_t record;
  if (mode) {
    int i = catalog_find(g_catalog,g_catalog_count,local_edit.id);
    if (i < 0 || g_catalog[i].slot != local_edit.slot || g_catalog[i].resource_state != 1 ||
        g_catalog[i].widget_checksum != local_edit.checksum) {
      local_edit.mode = 0; local_edit.failed = true; local_work_update(); ext_unlock(); miniapp_runtime_unlock(); return;
    }
    slot = g_catalog[i].slot;
    record = local_records[slot];
    if (record.mode == PET_ALARM_OFF && (mode == PET_ALARM_ALERT || mode == PET_ALARM_COUNT)) {
      pet_cev_clock_snapshot_t clock = miniapp_utc_snapshot(now);
      if (clock.valid)
        for (unsigned j=0; j<PET_CEV_ALARMS; j++)
          if (record.delivered[j]<clock.utc_ms) record.delivered[j]=clock.utc_ms;
    }
    if (mode <= PET_ALARM_ALERT) record.mode = mode;
#if PET_COMPONENT_BACKGROUND_ENABLED
    else {
      record.program = mode;
      record.buttons = g_catalog[i].buttons_checksum;
      record.sprites = g_catalog[i].sprites_checksum;
    }
#endif
    copy_utf8(record.id,sizeof(record.id),local_edit.id);
    record.checksum = g_catalog[i].widget_checksum;
  } else {
    pet_cev_clock_snapshot_t clock = miniapp_utc_snapshot(now);
    if (!local_job.pending || !clock.valid || clock.utc_ms < local_job.due) { ext_unlock(); miniapp_runtime_unlock(); return; }
    record = local_records[slot];
    record.delivered[local_job.index] = local_job.due;
    if (record.completed != UINT32_MAX) record.completed++;
  }
  ext_unlock(); miniapp_runtime_unlock();
  if (!pet_storage_scope_unlock(&storage)) return;
  bool ok = storage_work_current() && local_alarm_write(slot,&record);
#if PET_COMPONENT_BACKGROUND_ENABLED
  if (mode == PET_LOCAL_PROGRAM_OFF) {
    bool released = background_prepare_remove(record.id);
    if (released) {
      miniapp_runtime_lock();
      if (g_runtime.active && !strcmp(g_runtime.widget_id,record.id))
        g_runtime.program_background_owned = false;
      miniapp_runtime_unlock();
    }
    ok &= released;
  }
#endif
  if (ok) (void)pet_storage_scope_advance(&storage,PET_STORAGE_DURABLE);
  if (!pet_storage_scope_lock(&storage)) return;
  ext_lock();
  if (ok) local_records[slot] = record;
  if (mode) {
    local_edit.failed = !ok;
    /* Denial takes effect at input. Retry a failed durable denial or lease
     * cleanup without requiring a second physical toggle. */
    if (!ok && (mode == PET_ALARM_OFF || mode == PET_LOCAL_PROGRAM_OFF))
      local_edit.retry_after = now + 30000;
    else local_edit.mode = 0;
#if PET_COMPONENT_BACKGROUND_ENABLED
    if (ok && mode == PET_LOCAL_PROGRAM_ON) program_pending |= UINT64_C(1)<<slot;
#endif
  } else if (ok && local_job.pending && serial == local_job.serial) {
    pet_cev_clock_snapshot_t clock = miniapp_utc_snapshot(esp_timer_get_time()/1000ULL);
    if (clock.valid && clock.utc_ms >= local_job.due && record.mode == PET_ALARM_ALERT &&
        !g_ext_notice_pending && !local_edit.mode) {
      copy_utf8(g_ext_notice_id,sizeof(g_ext_notice_id),record.id);
      copy_utf8(g_ext_notice_title,sizeof(g_ext_notice_title),local_job.title);
      copy_utf8(g_ext_notice_text,sizeof(g_ext_notice_text),local_job.text);
      copy_utf8(g_ext_notice_alarm,sizeof(g_ext_notice_alarm),local_job.alarm);
      g_ext_notice_pending = true;
    }
    local_job.pending = false;
  }
  local_work_update(); ext_unlock();
  if (ok) {
    (void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);
    (void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);
  } else if (!mode) local_retry_ms = now + 30000;
}
