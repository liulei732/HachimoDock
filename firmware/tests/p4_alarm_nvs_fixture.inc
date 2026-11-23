typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, IO_ERROR, NVS_READONLY, NVS_READWRITE };
static unsigned char blobs[PET_P4_MINIAPP_CATALOG_MAX][256], staged[256];
static size_t sizes[PET_P4_MINIAPP_CATALOG_MAX], staged_size;
static unsigned write_slot, commits;
static bool fail_commit, namespace_missing;
static unsigned nvs_blob_reads;
static unsigned key_slot(const char *key){
 unsigned slot; assert(sscanf(key,"alarm%u",&slot)==1 && slot<PET_P4_MINIAPP_CATALOG_MAX);
 return slot;
}
static int nvs_open(const char *ns,int access,nvs_handle_t *h){
 assert(!model_depth&&!state_depth&&!strcmp(ns,"p4_alarms")); *h=access;
 return namespace_missing && access==NVS_READONLY ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
static void nvs_close(nvs_handle_t h){(void)h;}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size){
 assert(h==NVS_READONLY); ++nvs_blob_reads; unsigned slot=key_slot(key);
 if(!sizes[slot]) return ESP_ERR_NVS_NOT_FOUND;
 if(out){assert(*size>=sizes[slot]);memcpy(out,blobs[slot],sizes[slot]);}
 *size=sizes[slot];return ESP_OK;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size){
 assert(!state_depth&&!model_depth&&h==NVS_READWRITE && size<=sizeof staged);
 write_slot=key_slot(key); staged_size=size; memcpy(staged,data,size);return ESP_OK;
}
static int nvs_erase_key(nvs_handle_t h,const char *key){
 assert(h==NVS_READWRITE);write_slot=key_slot(key);staged_size=0;return ESP_OK;
}
static int nvs_commit(nvs_handle_t h){
 assert(h==NVS_READWRITE);if(fail_commit)return IO_ERROR;
 sizes[write_slot]=staged_size;memcpy(blobs[write_slot],staged,staged_size);commits++;return ESP_OK;
}
