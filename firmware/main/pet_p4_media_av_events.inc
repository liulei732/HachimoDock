/* Media lock owns this bounded queue. A route change or view transition
 * retires pending key presses; a reconnect only sees current visibility. */
static uint32_t media_view_epoch, media_view_boot, media_poll_generation, media_poll_assoc;
static int64_t media_poll_us;
static cJSON *media_keys[8];
static unsigned media_key_head, media_key_count;

static bool media_listener(void) {
  return media_poll_us && esp_timer_get_time()-media_poll_us < 3500000LL;
}
static void media_drop_keys(void) {
  while (media_key_count) {
    cJSON_Delete(media_keys[media_key_head]);
    media_key_head=(media_key_head+1)%8;
    --media_key_count;
  }
}
static void media_record_key(const char *topic,const cJSON *p) {
  if (!media_listener() || strcmp(topic,"media/event") || strncmp(visible_source,"pc.",3) ||
      strcmp(str(p,"source"),visible_source) || !strcmp(str(p,"operation"),"visibility") ||
      media_key_count>=8) return;
  cJSON *key=cJSON_Duplicate(p,true);
  if (key) {media_keys[(media_key_head+media_key_count)%8]=key; ++media_key_count;}
}
static bool media_radio_matches_locked(const char *source,const char *instance) {
  char token[24];
  snprintf(token,sizeof(token),"%08lx-%08lx",(unsigned long)media_view_boot,(unsigned long)media_view_epoch);
  return !strncmp(source,"pc.",3) && !strcmp(source,visible_source) && !strcmp(instance,token);
}
bool pet_p4_media_radio_matches(const char *source,const char *instance) {
  if (!lock) return false;
  xSemaphoreTake(lock,portMAX_DELAY);
  bool valid=media_radio_matches_locked(source,instance);
  xSemaphoreGive(lock);
  return valid;
}
