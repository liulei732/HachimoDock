/* Runs on the protocol storage lane. Incoming packages never supply a grant
 * stamp, transfer options, filesystem names, or a save record. */
static bool community_id_available(const char *id) {
  static const char *const reserved[]={"music-player","upcoming-todos","fruit-slice","touch-slice","bloomfrog_companion","falling-catch"};
  for (size_t i=0;i<sizeof(reserved)/sizeof(*reserved);i++)
    if (!strcmp(id,reserved[i])) return false;
  cJSON *index=cJSON_Parse(pet_p4_builtin_index_json);
  const cJSON *entries=cJSON_GetObjectItemCaseSensitive(index,"components"), *entry;
  bool ok=cJSON_IsArray(entries) && pet_p4_builtin_slice_count>0 &&
    (size_t)cJSON_GetArraySize(entries)==pet_p4_builtin_slice_count;
  cJSON_ArrayForEach(entry,entries) if (!strcmp(json_string(entry,"id"),id)) ok=false;
  cJSON_Delete(index);
  return ok;
}

bool pet_community_store(const pet_community_item_t *item,const char *expected,
    const pet_community_files_t *files,bool (*live)(void *),void *ctx) {
  if (!item || !expected || !live || !live(ctx) || !safe_widget_id(item->id) ||
      !pet_p4_miniapp_lan_install_idle() || g_catalog_recovery_blocked) return false;
  const int at=catalog_find(g_catalog,g_catalog_count,item->id);
  if (at<0) {
    if (*expected || !files || !community_id_available(item->id)) return false;
  } else {
    const miniapp_catalog_item_t *old=&g_catalog[at];
    if (!old->community_save[0] || !*expected || strcmp(old->package_metadata.version,expected)) return false;
    if (files && (!pet_community_newer(item->version,expected) ||
        strncmp(old->community_save,files->save_contract,32) ||
        (strlen(old->community_save)==64 && strcmp(old->community_save,files->save_contract)))) return false;
  }
  char error[160]={0};
  if (!files) return pet_p4_miniapp_remove_preserving_save(item->id,error,sizeof(error));
  if (files->buttons_len>=sizeof(g_staging.buttons_json) || files->widget_len>=sizeof(g_staging.widget_json)) return false;
  cJSON *request=cJSON_CreateObject();
  bool began=request && cJSON_AddStringToObject(request,"transferId","device-community") &&
    cJSON_AddStringToObject(request,"widgetId",item->id) && pet_p4_miniapp_install_begin(request,error,sizeof(error));
  bool ok=false;
  if (began) {
    memcpy(g_staging.widget_json,files->widget,files->widget_len);
    g_staging.widget_json[files->widget_len]=0;
    g_staging.widget_len=files->widget_len;
    memcpy(g_staging.buttons_json,files->buttons,files->buttons_len);
    g_staging.buttons_json[files->buttons_len]=0;
    g_staging.buttons_len=files->buttons_len;
    strcpy(g_staging.community_save,files->save_contract);
    strcpy(g_staging.package_metadata.version,item->version);
    strcpy(g_staging.package_metadata.kind,"tool");
    ok=install_commit_fenced(request,error,sizeof(error),live,ctx);
    if (transfer_matches(request)) widget_install_stage_reset();
  }
  cJSON_Delete(request);
  return ok;
}
