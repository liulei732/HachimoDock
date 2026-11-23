#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_COMMUNITY_ROWS 4
#define PET_COMMUNITY_BUTTONS_MAX 2047
#define PET_COMMUNITY_WIDGET_MAX 4095
#define PET_COMMUNITY_FRAME_OVERHEAD (sizeof("buttons.json") + sizeof("runtime/widget.json") + 8)
#define PET_COMMUNITY_PAYLOAD_MAX (PET_COMMUNITY_FRAME_OVERHEAD + PET_COMMUNITY_BUTTONS_MAX + PET_COMMUNITY_WIDGET_MAX)

typedef struct {
  char id[44], name[97], version[33], digest[65];
  unsigned bytes, privileges;
} pet_community_item_t;
typedef struct {
  pet_community_item_t items[PET_COMMUNITY_ROWS];
  unsigned count;
  char next[44];
} pet_community_catalog_t;
typedef struct {
  const uint8_t *buttons, *widget;
  size_t buttons_len, widget_len;
  char save_contract[65];
} pet_community_files_t;

bool pet_community_parse_catalog(const uint8_t *data, size_t size, const char *nonce,
    const char *after, pet_community_catalog_t *out);
bool pet_community_parse_payload(const pet_community_item_t *item, const uint8_t *data,
    size_t size, pet_community_files_t *files);
bool pet_community_hash(const void *data, size_t size, char out[65]);
bool pet_community_newer(const char *version, const char *installed);
bool pet_community_verify_catalog(const uint8_t *body, size_t size, const char *key,
    const char *nonce, const char *after, pet_community_catalog_t *out);

typedef struct {
  char title[97], version[33], id[44];
  const char *status;
  unsigned index, count, privileges;
  bool busy, more, install, remove, installed, added_save;
} pet_community_view_t;
void pet_community_view(pet_community_view_t *out);
void pet_community_browse(bool next);
void pet_community_move(int delta);
void pet_community_action(bool remove);
void pet_community_close(void);
void pet_community_local(void);
bool pet_community_schedule(void *job);
bool pet_community_https(const char *path, char *body, size_t limit, size_t *length,
    bool (*current)(void *), void *ctx);
bool pet_community_store(const pet_community_item_t *item, const char *expected,
    const pet_community_files_t *files, bool (*live)(void *), void *ctx);

void pet_community_run(void *job, bool admitted);
