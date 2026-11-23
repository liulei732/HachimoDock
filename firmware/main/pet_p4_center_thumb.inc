/* Launcher sidecar. Catalog publication selects an independently banked file,
 * so a thumbnail-only repair never writes package generations or user saves. */
static void center_thumb_path(unsigned slot,unsigned bank,bool temporary,char *path,size_t size) {
  snprintf(path,size,"/spiffs/p4th%02u-%u.%s",slot,bank,temporary ? "tmp" : "p4t");
}

static uint32_t center_thumb_hash(const uint8_t *data) {
  uint32_t hash = miniapp_checksum((const char *)data,PET_CENTER_THUMB_BYTES);
  return hash ? hash : 1;
}

bool pet_p4_miniapp_center_read_thumb(const pet_p4_miniapp_catalog_entry_t *entry,uint8_t *packed,size_t capacity) {
  if (!entry || !packed || !entry->center_thumb_checksum || capacity < PET_CENTER_THUMB_BYTES) return false;
  char path[40];
  center_thumb_path(entry->center_title_slot,entry->center_thumb_generation,false,path,sizeof(path));
  FILE *file = fopen(path,"rb");
  if (!file) return false;
  bool ok = fread(packed,1,PET_CENTER_THUMB_BYTES,file) == PET_CENTER_THUMB_BYTES && fgetc(file) == EOF && !ferror(file);
  fclose(file);
  return ok && center_thumb_hash(packed) == entry->center_thumb_checksum;
}

static bool center_thumb_matches(const miniapp_catalog_item_t *item) {
  if (!item->center_thumb_checksum || item->center_thumb_checksum != center_thumb_hash(g_staging.center_thumb)) return false;
  char path[40];
  center_thumb_path(item->slot,item->center_thumb_generation,false,path,sizeof(path));
  return pet_file_exact(path,g_staging.center_thumb,PET_CENTER_THUMB_BYTES,NULL,0,storage_work_current)==PET_FILE_EXACT_MATCH;
}

/* Remember both sealed catalogs. Three slots for thumbnail bytes suffice for a
 * current, rollback and proposed thumbnail, including across component-ID reuse. */
static uint8_t center_thumb_ref[2][PET_P4_MINIAPP_CATALOG_MAX]; /* bank + 1 */
static bool center_thumb_known[2];
static void center_thumb_catalog(unsigned generation,const miniapp_catalog_item_t *items,size_t count,bool committed) {
  uint8_t refs[PET_P4_MINIAPP_CATALOG_MAX]={0};
  for(size_t i=0;i<count;i++) if(items[i].slot<PET_P4_MINIAPP_CATALOG_MAX && items[i].center_thumb_checksum)
    refs[items[i].slot]=1+items[i].center_thumb_generation;
  for(unsigned slot=0;committed && center_thumb_known[1-generation] && slot<sizeof(refs);slot++){
    uint8_t old=center_thumb_ref[generation][slot];
    if(old && old!=refs[slot] && old!=center_thumb_ref[1-generation][slot] && storage_work_current()){
      char path[40];
      center_thumb_path(slot,old-1,false,path,sizeof(path));
      (void)remove_file_if_present(path);
    }
  }
  memcpy(center_thumb_ref[generation],refs,sizeof(refs));
  center_thumb_known[generation]=true;
}

static bool stage_center_thumb(miniapp_catalog_item_t *item) {
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  /* A full install after a failed unchanged proof must not reuse the same
   * stale cached thumbnail that physical verification just rejected. */
  if (g_staging.center_thumb_len && g_staging.preserve_unchanged && item->center_thumb_checksum) {
    pet_storage_physical_scope_t physical={0};
    if (storage_audio_busy() || !storage_work_current() || !pet_storage_physical_begin(&physical)) return false;
    bool same=center_thumb_matches(item);
    if (!pet_storage_physical_finish(&physical) || storage_audio_busy() || !storage_work_current()) return false;
    if (same) return true;
  } else
#endif
  if (!g_staging.center_thumb_len || center_thumb_matches(item)) return true;
  char path[40],tmp[40];
  unsigned bank=0;
  for(;bank<3;bank++){
    if (center_thumb_ref[0][item->slot]==bank+1 || center_thumb_ref[1][item->slot]==bank+1 ||
        (item->center_thumb_checksum && item->center_thumb_generation==bank)) continue;
    if (!center_thumb_known[0] || !center_thumb_known[1]) {
      /* An unreadable catalog can still have a valid backup. Only use a fresh name. */
      struct stat st;
      center_thumb_path(item->slot,bank,false,path,sizeof(path));
      if (stat(path,&st)==0 || errno!=ENOENT) continue;
    }
    break;
  }
  if(bank==3)return false;
  center_thumb_path(item->slot,bank,false,path,sizeof(path));
  center_thumb_path(item->slot,bank,true,tmp,sizeof(tmp));
  if (!write_file_atomic(tmp,path,(const char *)g_staging.center_thumb,PET_CENTER_THUMB_BYTES) ||
      pet_file_exact(path,g_staging.center_thumb,PET_CENTER_THUMB_BYTES,NULL,0,storage_work_current)!=PET_FILE_EXACT_MATCH) return false;
  item->center_thumb_checksum = center_thumb_hash(g_staging.center_thumb);
  item->center_thumb_generation = bank;
  return true;
}
