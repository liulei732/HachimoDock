/* Called with the protocol state lock held, including the render snapshot check.
 * Only a fixed local notice can be shown. No text, keys or sessions go on the wire. */
static struct {
  char intent[33];
  char agent[PET_P4_AGENT_MAX], session_agent[PET_P4_AGENT_MAX], session[PET_P4_SESSION_QUEUE_ID_MAX];
  uint64_t route, deadline, visible_until;
  uint32_t token;
  bool delivered;
} test_message;

static bool test_message_same_view(const pet_p4_runtime_state_t *s) {
  return !strcmp(s->screen_page, "main") && !s->approval_active && !s->session_voice_active &&
    !pet_p4_conversation_active(s) && !s->asset_transfer_active &&
    !strcmp(s->active_agent, test_message.agent) &&
    !strcmp(s->current_session_agent, test_message.session_agent) &&
    !strcmp(s->current_session_id, test_message.session);
}

void pet_p4_test_message_check_view(const pet_p4_runtime_state_t *s) {
  if (test_message.intent[0] && !test_message_same_view(s)) {
    test_message.intent[0] = 0;
    test_message.visible_until = 0;
  }
}

bool pet_p4_test_message_visible(const pet_p4_runtime_state_t *s, unsigned long long now) {
  pet_p4_test_message_check_view(s);
  return test_message.visible_until > now;
}

cJSON *pet_p4_test_message(pet_p4_runtime_state_t *s, const cJSON *q, uint64_t route, bool admitted) {
  const char *id = json_string(q, "intent"), *phase = json_string(q, "phase");
  const char *boot = pet_p4_boot_identity();
  const bool prepare = !strcmp(phase, "prepare"), send = !strcmp(phase, "send");
  const bool wired = route == 0;
  const uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
  const cJSON *token = cJSON_GetObjectItemCaseSensitive(q, "token");
  const char *result = "refused";
  const char *rid = json_string(q, "requestId");
  const bool valid_rid = wired && *rid && strlen(rid) <= 56;
  const bool identity = strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32 &&
    boot && strlen(boot) == 16 && !strcmp(boot, json_string(q, "bootId")) &&
    s->board_device_id[0] && !strcmp(s->board_device_id, json_string(q, "boardDeviceId"));
  if (identity && (prepare || send) && cJSON_GetArraySize(q) == 4 + wired + send &&
      (!wired || valid_rid) && (prepare ? !token : cJSON_IsNumber(token))) {
    if (!admitted || strcmp(s->screen_page, "main") || s->approval_active ||
        s->session_voice_active || pet_p4_conversation_active(s) || s->asset_transfer_active ||
        (s->voice_feedback.phase != PET_VOICE_UNKNOWN && s->voice_feedback.phase != PET_VOICE_IDLE) ||
        (prepare && test_message.visible_until > now && test_message_same_view(s))) {
      result = "busy";
    } else if (prepare) {
      if (++test_message.token == 0) ++test_message.token;
      snprintf(test_message.intent, sizeof(test_message.intent), "%s", id);
      snprintf(test_message.agent, sizeof(test_message.agent), "%s", s->active_agent);
      snprintf(test_message.session_agent, sizeof(test_message.session_agent), "%s", s->current_session_agent);
      snprintf(test_message.session, sizeof(test_message.session), "%s", s->current_session_id);
      test_message.route = route;
      test_message.deadline = now + 5000;
      test_message.delivered = false;
      test_message.visible_until = 0;
      result = "ready";
    } else if (strcmp(id, test_message.intent) || route != test_message.route ||
               token->valuedouble != test_message.token || !test_message_same_view(s)) {
      result = "changed";
    } else if (test_message.delivered) {
      result = "delivered"; /* Same receipt, no second display or extension. */
    } else if (now >= test_message.deadline) {
      result = "expired";
    } else {
      test_message.delivered = true;
      test_message.visible_until = now + 3000;
      result = "delivered";
    }
  }
  cJSON *r = cJSON_CreateObject();
  if (!r || !cJSON_AddBoolToObject(r, "ok", true) ||
      !cJSON_AddStringToObject(r, "boardDeviceId", s->board_device_id) ||
      !cJSON_AddStringToObject(r, "bootId", boot ? boot : "") ||
      !cJSON_AddStringToObject(r, "intent", identity ? id : "") ||
      !cJSON_AddNumberToObject(r, "token", test_message.token) ||
      !cJSON_AddStringToObject(r, "status", result) ||
      (wired && !cJSON_AddStringToObject(r, "requestId", valid_rid ? rid : ""))) {
    cJSON_Delete(r);
    return NULL;
  }
  return r;
}
