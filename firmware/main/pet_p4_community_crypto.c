#include "pet_p4_community.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include <stdlib.h>
#include <string.h>

extern cJSON *pet_community_json(const uint8_t *bytes, size_t size);

bool pet_community_hash(const void *data, size_t size, char out[65]) {
  static const char hex[]="0123456789abcdef";
  uint8_t digest[32];
  if (mbedtls_sha256(data,size,digest,0)) return false;
  for (unsigned i=0;i<32;i++) {
    out[2*i]=hex[digest[i]>>4];
    out[2*i+1]=hex[digest[i]&15];
  }
  out[64]=0;
  return true;
}

static bool decode(const char *s,uint8_t *out,size_t cap,size_t *n) {
  if (!s || !*s || strlen(s)>4*((cap+2)/3) || strlen(s)%4) return false;
  return !mbedtls_base64_decode(out,cap,n,(const uint8_t *)s,strlen(s)) && *n;
}

bool pet_community_verify_catalog(const uint8_t *body,size_t size,const char *key,
    const char *nonce,const char *after,pet_community_catalog_t *out) {
  if (!key || !key[0] || !out || size>8192) return false;
  /* Fixed EC public-key and prime256v1 OIDs, followed by uncompressed Q. */
  static const uint8_t prefix[]={0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,
    0x02,0x01,0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00,0x04};
  uint8_t pub[91],sig[72],hash[32];
  size_t n=0,siglen=0,bytes=0;
  cJSON *root=pet_community_json(body,size);
  const cJSON *data=cJSON_GetObjectItemCaseSensitive(root,"data");
  const cJSON *signature=cJSON_GetObjectItemCaseSensitive(root,"signature");
  uint8_t *raw=malloc(4096);
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool ok=raw && cJSON_IsObject(root) && cJSON_GetArraySize(root)==2 &&
    cJSON_IsString(data) && cJSON_IsString(signature) &&
    decode(key,pub,sizeof(pub),&n) && n==sizeof(pub) && !memcmp(pub,prefix,sizeof(prefix)) &&
    decode(signature->valuestring,sig,sizeof(sig),&siglen) && decode(data->valuestring,raw,4096,&bytes) &&
    !mbedtls_pk_parse_public_key(&pk,pub,n) && !mbedtls_sha256(raw,bytes,hash,0) &&
    !mbedtls_pk_verify(&pk,MBEDTLS_MD_SHA256,hash,sizeof(hash),sig,siglen) &&
    pet_community_parse_catalog(raw,bytes,nonce,after,out);
  mbedtls_pk_free(&pk);
  cJSON_Delete(root);
  free(raw);
  return ok;
}
