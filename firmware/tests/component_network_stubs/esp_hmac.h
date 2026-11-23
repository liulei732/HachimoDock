#include <stddef.h>
#include <stdint.h>
typedef int hmac_key_id_t;
int esp_hmac_calculate(hmac_key_id_t, const void *, size_t, uint8_t *);
