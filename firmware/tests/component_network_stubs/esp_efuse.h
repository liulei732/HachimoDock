#pragma once
#include <stdbool.h>
typedef int esp_efuse_block_t;
enum { EFUSE_BLK_KEY0 = 4, ESP_EFUSE_KEY_PURPOSE_HMAC_UP = 8 };
bool esp_efuse_find_purpose(int, esp_efuse_block_t *);
bool esp_efuse_get_key_dis_read(esp_efuse_block_t);
bool esp_efuse_get_key_dis_write(esp_efuse_block_t);
bool esp_efuse_get_keypurpose_dis_write(esp_efuse_block_t);
