/* Single producer (codec task), single authenticated consumer. Only an ACK
 * for a frame actually offered to this reader releases retained samples. */
#define NOTE_SLOTS 64
#define NOTE_FRAME_MAX 1536
#define NOTE_BATCH 8
static SemaphoreHandle_t note_lock;
static char note_owner[64];
static char *note_frames[NOTE_SLOTS];
static unsigned note_ack, note_tail, note_offered;
static bool note_fault;
static unsigned note_pcm_bytes;
static int64_t note_until;

static void note_release(unsigned cursor) {
  while (note_ack < cursor) {
    char **p = &note_frames[note_ack++ % NOTE_SLOTS];
    if (*p) { mbedtls_platform_zeroize(*p, strlen(*p)); cJSON_free(*p); *p = NULL; }
  }
}

static bool note_publish(const char *owner, const char *topic, cJSON *payload) {
  if (!cJSON_GetObjectItemCaseSensitive(payload, "boardDeviceId"))
    cJSON_AddStringToObject(payload, "boardDeviceId", g_board_device_id);
  unsigned pcm = !strcmp(topic,"audio/chunk") ? PET_P4_AUDIO_FRAME_BYTES : 0;
  cJSON *root = cJSON_CreateObject();
  bool ok = root && cJSON_AddStringToObject(root, "topic", topic) &&
            cJSON_AddItemToObject(root, "payload", payload);
  if (!ok) cJSON_Delete(payload);
  char *line = ok ? cJSON_PrintUnformatted(root) : NULL;
  cJSON_Delete(root);
  xSemaphoreTake(note_lock, portMAX_DELAY);
  ok = !strcmp(owner, note_owner) && !note_fault && line && strlen(line) <= NOTE_FRAME_MAX &&
       note_tail - note_ack < NOTE_SLOTS && note_tail < 1600 &&
       esp_timer_get_time() < note_until && pcm <= 960000U-note_pcm_bytes;
  if (ok) { note_pcm_bytes += pcm; note_frames[note_tail++ % NOTE_SLOTS] = line; line = NULL; }
  else if (!strcmp(owner, note_owner)) note_fault = true;
  xSemaphoreGive(note_lock);
  if (line) { mbedtls_platform_zeroize(line, strlen(line)); cJSON_free(line); }
  return ok;
}

static bool note_reserve(const char *owner) {
  /* Reservation is called only by the serialized LAN server. */
  if (!note_lock) note_lock = xSemaphoreCreateMutex();
  if (!note_lock) return false;
  xSemaphoreTake(note_lock, portMAX_DELAY);
  bool ok = !note_owner[0];
  if (ok) {
    strcpy(note_owner, owner);
    note_ack = note_tail = note_offered = 0;
    note_fault = false;
    note_pcm_bytes=0; note_until=esp_timer_get_time()+3500000LL;
  }
  xSemaphoreGive(note_lock);
  return ok;
}

void pet_p4_note_discard(const char *owner) {
  if (!note_lock) return;
  xSemaphoreTake(note_lock, portMAX_DELAY);
  if (!strcmp(owner, note_owner)) { note_release(note_tail); note_owner[0] = 0; }
  xSemaphoreGive(note_lock);
}

cJSON *pet_p4_note_read(const char *owner, unsigned cursor) {
  if (!note_lock) return NULL;
  xSemaphoreTake(note_lock, portMAX_DELAY);
  cJSON *reply = NULL;
  if (!strcmp(owner, note_owner) && !note_fault && cursor >= note_ack && cursor <= note_offered) {
    note_release(cursor);
    reply = cJSON_CreateObject();
    cJSON *frames = reply ? cJSON_AddArrayToObject(reply, "frames") : NULL;
    unsigned pos = cursor;
    bool ok = frames && cJSON_AddBoolToObject(reply, "ok", true) && cJSON_AddNumberToObject(reply, "cursor", cursor);
    while (ok && pos < note_tail && pos - cursor < NOTE_BATCH) {
      cJSON *frame = cJSON_Parse(note_frames[pos % NOTE_SLOTS]);
      ok = frame && cJSON_AddItemToArray(frames, frame);
      if (!ok) cJSON_Delete(frame);
      ++pos;
    }
    if (ok) {note_offered = pos; note_until=esp_timer_get_time()+3500000LL;}
    else { cJSON_Delete(reply); reply = NULL; }
  }
  xSemaphoreGive(note_lock);
  return reply;
}
