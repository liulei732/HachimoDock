/* Native component account panel is the sole producer. One low-priority
 * worker owns this immutable request until it publishes its final result. */
static struct {
  pet_private_feed_t binding;
  pet_wifi_endpoint_t endpoint;
  uint32_t epoch;
} write_job;
static atomic_int write_result;
static atomic_bool write_cancelled;

static bool write_current(void *unused) {
  (void)unused;
  return !atomic_load(&write_cancelled) && pet_device_wifi_online() &&
         write_job.epoch == pet_private_storage_epoch();
}

typedef struct {
  esp_tls_t *tls;
  const void *bytes;
  size_t size;
} write_chunk_t;

static int send_authorized_chunk(void *context) {
  write_chunk_t *chunk = context;
  if (!pet_write_guard(write_job.epoch)) return -1;
  int result = -1;
  if (!atomic_load(&write_cancelled))
    result = esp_tls_conn_write(chunk->tls, chunk->bytes, chunk->size);
  pet_write_unlock();
  return result;
}

static ssize_t guarded_write(esp_tls_t *tls, const void *bytes, size_t size) {
  write_chunk_t chunk = {tls, bytes, size};
  return pet_p4_miniapp_write_if_active(write_job.binding.id, send_authorized_chunk, &chunk);
}

static bool submit_https(mqtt_feed_t *feed, const pet_private_secret_t *secret) {
  char request[640];
  const char *value = write_job.binding.value;
  const int n = snprintf(request, sizeof(request),
      "POST %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
      "Content-Type: text/plain\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
      write_job.endpoint.path, write_job.endpoint.host, secret->token, (unsigned)strlen(value), value);
  const int64_t until = clock_ms() + IO_MS;
  const bool sent = n > 0 && (size_t)n < sizeof(request) &&
      transfer(feed, request, n, true, until) == 1;
  mbedtls_platform_zeroize(request, sizeof(request));
  if (!sent) return false;
  char line[128];
  for (size_t i = 0; i < sizeof(line); ++i) {
    if (transfer(feed, line + i, 1, false, until) != 1) break;
    if ((unsigned char)line[i] < 32 && line[i] != '\r' && line[i] != '\n' && line[i] != '\t') break;
    if (i && line[i-1] == '\r' && line[i] != '\n') break;
    if (line[i] == '\n') return i >= 13 && line[i-1] == '\r' &&
      (!memcmp(line, "HTTP/1.1 ", 9) || !memcmp(line, "HTTP/1.0 ", 9)) &&
      line[9] == '2' && line[10] >= '0' && line[10] <= '9' &&
      line[11] >= '0' && line[11] <= '9' && line[12] == ' ';
  }
  return false;
}

static bool mqtt_reply(mqtt_feed_t *feed, const uint8_t expected[4]) {
  uint8_t reply[4];
  return transfer(feed, reply, sizeof(reply), false, clock_ms() + IO_MS) == 1 &&
         !memcmp(reply, expected, sizeof(reply));
}

static bool publish_once(mqtt_feed_t *feed) {
  uint8_t packet[2 + 40 + 2 + 24];
  uint8_t *end = put_utf8(packet, write_job.binding.topic);
  *end++ = 0;
  *end++ = 1;
  const size_t length = strlen(write_job.binding.value);
  memcpy(end, write_job.binding.value, length);
  return send_packet(feed, 0x32, packet, end - packet + length) &&
         mqtt_reply(feed, (uint8_t[]){0x40, 2, 0, 1});
}

static bool write_online(void) {
  uint64_t utc, age;
  int32_t offset;
  return write_current(NULL) && pet_wifi_clock_read(clock_ms(), &utc, &offset, &age) &&
         age <= 86400000ULL && time(NULL) >= 1704067200;
}


static int may_claim_write(void *unused) {
  (void)unused;
  return write_current(NULL);
}

static void device_write_worker(void *unused) {
  (void)unused;
  const bool mqtt = write_job.binding.kind == PET_MQTT_PUBLISH;
  pet_private_secret_t secret = {0};
  mqtt_feed_t feed = {.current = write_current, .guarded_write = guarded_write};
  pet_private_result_t result = pet_private_secret_read(&write_job.binding, &secret);
  if (result != PET_PRIVATE_SET) goto done;
  result = PET_PRIVATE_REFUSED;
  if (!write_online()) goto done;
  const esp_tls_cfg_t config = {.non_block = true, .timeout_ms = IO_MS,
                                 .crt_bundle_attach = esp_crt_bundle_attach};
  feed.tls = pet_live_tls_connect_public_port(write_job.endpoint.host, write_job.binding.port,
      &config, clock_ms() + IO_MS, write_current, NULL);
  if (!feed.tls || (mqtt && (!connect_packet(&feed, &secret) ||
      !mqtt_reply(&feed, (uint8_t[]){0x20, 2, 0, 0}))) ||
      pet_p4_miniapp_write_if_active(write_job.binding.id, may_claim_write, NULL) != 1) goto done;

  /* Commit the spent grant before any bytes of the mutating request. A reset
   * at any later instruction is ambiguous and must never replay the request. */
  result = pet_write_claim(&write_job.binding, write_job.epoch, &secret);
  if (result != PET_PRIVATE_SET) goto done;
  result = PET_WRITE_UNKNOWN;
  if (mqtt ? publish_once(&feed) : submit_https(&feed, &secret)) {
    if (pet_write_ack(&write_job.binding, write_job.epoch)) result = PET_WRITE_ACCEPTED;
  }
done:
  if (feed.tls) esp_tls_conn_destroy(feed.tls);
  mbedtls_platform_zeroize(&secret, sizeof(secret));
  atomic_store(&write_result, result);
  vTaskDelete(NULL);
}

bool pet_device_write_start(const pet_private_feed_t *selected) {
  if (!selected || atomic_load(&write_result) == PET_PRIVATE_BUSY) return false;
  if (!pet_feed_is_write(selected->kind)) return false;
  const uint32_t epoch = pet_private_storage_epoch();
  if (!pet_p4_miniapp_private_feed(&write_job.binding, &write_job.endpoint) ||
      memcmp(selected->id, write_job.binding.id, sizeof(selected->id))) return false;
  write_job.epoch = epoch;
  atomic_store(&write_cancelled, false);
  atomic_store(&write_result, PET_PRIVATE_BUSY);
  if (xTaskCreate(device_write_worker, "pet_write", 8192, NULL, 2, NULL) == pdPASS) return true;
  atomic_store(&write_result, PET_PRIVATE_REFUSED);
  return false;
}

void pet_device_write_cancel(void) { atomic_store(&write_cancelled, true); }
pet_private_result_t pet_device_write_result(void) { return atomic_load(&write_result); }
