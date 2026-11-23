#include "pet_p4_shared_font.h"
#include "pet_p4_shared_font_id.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "miniz.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define FONT_FILE "/spiffs/bubble32-v1.fpk"
#define COUNT 6988u
#define BLOCKS ((COUNT+63u)/64u)
static uint16_t map[COUNT];
static uint32_t offsets[BLOCKS];
static _Atomic(uint8_t *) glyphs;
static const uint8_t expected_hash[32] = PET_SHARED_FONT_DIGEST;

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
bool pet_shared_font_ready(void) { return glyphs != NULL; }

/* Authenticate before interpreting any glyph offsets. Reuse the same bounded
 * scratch for digest input, then for one compressed block at a time. */
#define SCRATCH_BYTES 16384u
static bool verify_file(FILE *file, uint8_t *scratch) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
    for (size_t left = PET_SHARED_FONT_BYTES; ok && left;) {
        size_t n = left < SCRATCH_BYTES ? left : SCRATCH_BYTES;
        ok = fread(scratch, 1, n, file) == n && !mbedtls_sha256_update(&sha, scratch, n);
        left -= n;
    }
    uint8_t digest[32];
    ok = ok && fgetc(file) == EOF && !ferror(file) && !mbedtls_sha256_finish(&sha, digest)
        && !memcmp(digest, expected_hash, sizeof(digest)) && fseek(file, 128, SEEK_SET) == 0;
    mbedtls_sha256_free(&sha);
    return ok;
}

static bool load_resource(const char *path, bool staged) {
    if (pet_shared_font_ready()) return true;
    FILE *f = fopen(path,"rb");
    if (!f) return false;
    const int64_t started = esp_timer_get_time();
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *scratch = heap_caps_malloc(SCRATCH_BYTES, caps);
    uint8_t *directory = NULL, *raw = NULL;
    tinfl_decompressor *inflater = NULL;
    bool ok = scratch && verify_file(f,scratch);
    if (ok) {
        directory = heap_caps_malloc(16*BLOCKS, caps);
        raw = heap_caps_malloc(PET_SHARED_FONT_RAW, caps);
        inflater = heap_caps_malloc(sizeof(*inflater), caps);
        ok = raw && inflater && directory && fread(map,1,sizeof(map),f)==sizeof(map)
            && fread(directory,16,BLOCKS,f)==BLOCKS;
    }
    for (unsigned i=0; ok && i<BLOCKS; ++i) {
        const uint8_t *d = directory + 16*i;
        size_t in = u32(d+4), out = u32(d+8);
        offsets[i] = u32(d+12);
        if (in>SCRATCH_BYTES || offsets[i]>PET_SHARED_FONT_RAW || out>PET_SHARED_FONT_RAW-offsets[i]
            || fread(scratch,1,in,f)!=in) { ok=false; break; }
        uint8_t *dst=raw+offsets[i];
        tinfl_init(inflater);
        ok = tinfl_decompress(inflater,scratch,&in,dst,dst,&out,
            TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)==TINFL_STATUS_DONE
            && in==u32(d+4) && out==u32(d+8);
    }
    fclose(f);
    if (ok && staged) {
        /* An invalid resource may remain from an older installation. Keep it
         * until the replacement has passed digest and decompression checks. */
        remove(FONT_FILE);
        ok = rename(path, FONT_FILE) == 0;
    }
    if (ok) {
        glyphs=raw;
    } else free(raw);
    free(inflater);
    free(directory);
    free(scratch);
    ESP_LOGI("shared-font","load staged=%d ok=%d elapsed_ms=%u largest_psram=%u",
        staged, ok, (unsigned)((esp_timer_get_time()-started)/1000),
        (unsigned)heap_caps_get_largest_free_block(caps));
    return ok;
}

void pet_shared_font_load(void) {
    remove("/spiffs/bubble32-v1.tmp");
    (void)load_resource(FONT_FILE, false);
}
bool pet_shared_font_activate_staged(const char *path) { return load_resource(path, true); }

bool pet_shared_font_lookup(uint32_t cp, pet_shared_glyph_t *glyph) {
    const uint8_t *pixels = glyphs;
    if (!pixels || cp>0xffff) return false;
    unsigned lo=0, hi=COUNT;
    while (lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if (map[mid]<cp) lo=mid+1;
        else hi=mid;
    }
    if (lo>=COUNT || map[lo]!=cp) return false;
    unsigned block=lo/64, n=COUNT-block*64;
    if (n>64) n=64;
    const uint8_t *base=pixels+offsets[block], *m=base+(lo%64)*8;
    *glyph=(pet_shared_glyph_t){base+n*8+m[0]+((unsigned)m[1]<<8),m[4],m[5],(int8_t)m[6],(int8_t)m[7]};
    return true;
}
