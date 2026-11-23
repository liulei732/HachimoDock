/* Called only after authenticated decryption. Recheck the session at dispatch
 * and on the protocol owner before returning a receipt or publishing storage. */
static void lan_maintenance_handle(int fd, mbedtls_gcm_context *gcm, const uint8_t header[3],
                                   uint8_t *cipher, uint8_t *plain, size_t size,
                                   uint32_t request_generation, int64_t deadline) {
  if (size < 36 || size > 548) return;
  const uint32_t association = bundle_u32(plain + 31);
  const unsigned op = plain[35];
  uint8_t mac[6];
  char board[16];
  if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) return;
  snprintf(board,sizeof(board),"p4-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  const bool current = association && association == pet_device_wifi_association() &&
    !memcmp(plain + 16, board, 15) && !atomic_load(&usb_client_active) &&
    !pet_p4_ota_active() && !pet_p4_diagnostics_reboot_pending() && bundle_wire_admit(plain,request_generation);
  if (op == 5) {
    const bool accepted = current && size == 36;
    uint8_t ready = accepted;
    if (send_sealed_response(fd,gcm,header,cipher,&ready,1,deadline) && accepted)
      bundle_wire_session_kind(fd,gcm,cipher,plain,request_generation,association,true);
    return;
  }
  size_t n = 1;
  cJSON *reply = NULL;
  if (current && op < 5 && size > 36 && !memchr(plain+36,0,size-36) &&
      pet_json_source_text_limit_bytes(plain+36,size-36,2)) {
    plain[size]=0;
    cJSON *q = cJSON_ParseWithLengthOpts((char *)plain+36,size-35,NULL,true);
    const char *rid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q,"requestId"));
    if (cJSON_IsObject(q) && rid && strlen(rid)==32 && strspn(rid,"0123456789abcdef")==32 &&
        (op || cJSON_GetArraySize(q)==1))
      control_deliver(fd,LAN_MAINTENANCE_MODE,plain,size,request_generation,q,&reply,NULL);
    cJSON_Delete(q);
  }
  if (reply && cJSON_PrintPreallocated(reply,(char *)plain,LAN_MAX_BODY-2,false)) n=strlen((char *)plain);
  else plain[0]=0;
  cJSON_Delete(reply);
  send_sealed_response(fd,gcm,header,cipher,plain,n,esp_timer_get_time()+LAN_IO_US);
}
