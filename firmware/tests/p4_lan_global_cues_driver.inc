/* Host adapter for already authenticated mode-18 bodies; production phase
 * dispatch, ownership, WAV verifier and filesystem writer are compiled above. */
int main(void) {
  strcpy(state.board_device_id,"fixture-board");
  char line[16384];
  (void)begin_request; (void)dispatch; (void)audio_patch_protocol_active;
  while(fgets(line,sizeof(line),stdin)) {
    char *json;
    unsigned phase=(unsigned)strtoul(line,&json,10);
    cJSON_Delete(last_ack);last_ack=NULL;
    if(phase==99) {
      peer_live=false; now_us+=1000000000;
      audio_patch_timeout_worker(&state);
      peer_live=true;
    } else {
      const size_t len=strlen(json);
      assert(len && len+17<16384);
      if(phase<=PET_LAN_AUDIO_ABORT && pet_json_source_text_limit_bytes(json,len,phase==PET_LAN_AUDIO_BEGIN?3:2)){
        cJSON *p=cJSON_Parse(json);
        if(p)pet_p4_handle_lan_appearance(&state,phase,p,(void*)2);
        cJSON_Delete(p);
      }
    }
    char *out=last_ack ? cJSON_PrintUnformatted(last_ack) : NULL;
    puts(out?out:"null");fflush(stdout);cJSON_free(out);
  }
  cJSON_Delete(last_ack);return 0;
}
