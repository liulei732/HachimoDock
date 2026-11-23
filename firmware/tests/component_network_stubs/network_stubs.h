#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_ANY 0
#define NVS_TYPE_BLOB 1
#define ESP_EVENT_ANY_ID -1
#define HTTP_TRANSPORT_OVER_SSL 1
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define MQTT_TRANSPORT_OVER_SSL 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef void *esp_http_client_handle_t;
typedef void *esp_mqtt_client_handle_t;
typedef const char *esp_event_base_t;
typedef struct {
  const char *host, *path, *common_name;
  int port, transport_type, timeout_ms, max_authorization_retries, buffer_size, buffer_size_tx;
  bool disable_auto_redirect;
  void *crt_bundle_attach;
} esp_http_client_config_t;
typedef struct {
  struct {
    struct {
      const char *hostname;
      int port, transport;
    } address;
    struct {
      void *crt_bundle_attach;
      const char *common_name;
    } verification;
  } broker;
  struct {
    const char *client_id, *username;
    struct {
      const char *password;
    } authentication;
  } credentials;
  struct {
    int keepalive;
  } session;
  struct {
    int timeout_ms, reconnect_timeout_ms;
  } network;
  struct {
    int size;
  } buffer;
  struct {
    int stack_size;
  } task;
  struct {
    int limit;
  } outbox;
} esp_mqtt_client_config_t;
typedef struct {
  int msg_id, data_len, total_data_len, topic_len, current_data_offset;
  char *data, *topic;
} esp_mqtt_event_t, *esp_mqtt_event_handle_t;
enum {
  MQTT_EVENT_CONNECTED = 1,
  MQTT_EVENT_DISCONNECTED,
  MQTT_EVENT_SUBSCRIBED,
  MQTT_EVENT_UNSUBSCRIBED,
  MQTT_EVENT_PUBLISHED,
  MQTT_EVENT_ERROR,
  MQTT_EVENT_DATA
};
typedef int nvs_handle_t;
typedef struct iterator *nvs_iterator_t;
typedef struct {
  int type;
  char key[16];
} nvs_entry_info_t;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_entry_find(const char *, const char *, int, nvs_iterator_t *);
esp_err_t nvs_entry_next(nvs_iterator_t *);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t *);
void nvs_release_iterator(nvs_iterator_t);
void esp_crt_bundle_attach(void);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
int esp_http_client_set_method(esp_http_client_handle_t, int);
int esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
void esp_http_client_cleanup(esp_http_client_handle_t);
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
int esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int,
                                   void (*)(void *, esp_event_base_t, int32_t, void *), void *);
int esp_mqtt_client_start(esp_mqtt_client_handle_t);
int esp_mqtt_client_stop(esp_mqtt_client_handle_t);
int esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t, const char *);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *, const char *, int, int, int);
int64_t esp_timer_get_time(void);
void vTaskDelete(void *);
void vTaskDelay(int);
int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *);
int mbedtls_sha256(const unsigned char *, size_t, unsigned char *, int);
