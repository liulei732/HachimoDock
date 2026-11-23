#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Use the production store with its SPIFFS paths relative to the harness cwd. */
static const char *local_path(const char *path) {
  assert(!strncmp(path, "/spiffs/", 8));
  return path + 8;
}
static FILE *local_open(const char *path, const char *mode) { return fopen(local_path(path), mode); }
static int local_rename(const char *old, const char *new) {
  /* SPIFFS rejects overwriting an existing name. */
  assert(access(local_path(new), F_OK) != 0);
  return rename(local_path(old), local_path(new));
}
static unsigned unlink_calls, scan_calls;
static unsigned fail_after_unlinks;
static int local_unlink(const char *path) {
  ++unlink_calls;
  if (fail_after_unlinks && unlink_calls > fail_after_unlinks) { errno=EIO; return -1; }
  return unlink(local_path(path));
}
static int local_stat(const char *path, struct stat *out) { return stat(local_path(path), out); }
static DIR *local_opendir(const char *path) {
  assert(!strcmp(path, "/spiffs"));
  ++scan_calls;
  return opendir(".");
}
static unsigned fail_scans;
static struct dirent *local_readdir(DIR *dir) {
  if (fail_scans && scan_calls <= fail_scans) { errno=EIO; return NULL; }
  return readdir(dir);
}
#define readdir local_readdir
#define opendir local_opendir
#define fopen local_open
#define rename local_rename
#define unlink local_unlink
#define stat(path, out) local_stat(path, out)
#include "pet_p4_component_media.c"
#undef opendir
#undef readdir
#undef fopen
#undef rename
#undef unlink
#undef stat

static void put_u32(uint8_t *out, uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) out[byte] = (uint8_t)(value >> (byte * 8));
}

enum { TEST_CUES = 8, TEST_BLOCKS = 394 };
static const size_t bank_size = 8 + TEST_CUES * (8 + TEST_BLOCKS * PET_CEXT_SPEECH_BLOCK_BYTES);

static uint8_t *make_bank(uint8_t predictor) {
  uint8_t *bank = calloc(1, bank_size);
  assert(bank);
  memcpy(bank, "HAB1", 4);
  put_u32(bank + 4, TEST_CUES);
  uint32_t next = 8 + TEST_CUES * 8;
  const unsigned length = TEST_BLOCKS * PET_CEXT_SPEECH_BLOCK_BYTES;
  for (unsigned i = 0; i < TEST_CUES; ++i) {
    put_u32(bank + 8 + i * 8, next);
    put_u32(bank + 12 + i * 8, length);
    for (unsigned block = 0; block < TEST_BLOCKS; ++block) {
      bank[next] = predictor;
      bank[next + 4] = 64;
      bank[next + 5] = 1;
      next += PET_CEXT_SPEECH_BLOCK_BYTES;
    }
  }
  assert(next == bank_size);
  return bank;
}

static void set_hash(pet_cext_runtime_t *runtime, const uint8_t *bank) {
  uint8_t hash[32];
  assert(mbedtls_sha256(bank, bank_size, hash, 0) == 0);
  for (unsigned i = 0; i < 32; ++i)
    snprintf(runtime->speech_sha256 + i * 2, 3, "%02x", hash[i]);
}

static void stage(const uint8_t *bank) {
  pet_cmedia_stage_reset();
  assert(!pet_cmedia_speech_stage(PET_CEXT_SPEECH_MAX_BYTES + 1, 0, bank, 4096, NULL, 0));
  assert(!pet_cmedia_speech_stage(bank_size, 4096, bank, 4096, NULL, 0));
  for (size_t offset = 0; offset < bank_size;) {
    size_t length = bank_size - offset;
    if (length > 4096) length = 4096;
    assert(pet_cmedia_speech_stage(bank_size, offset, bank + offset, length, NULL, 0));
    /* Repeated or changed-size chunks cannot move staged bytes. The widget
     * transport handles replay before re-entering this append path. */
    assert(!pet_cmedia_speech_stage(bank_size, offset, bank + offset, length, NULL, 0));
    offset += length;
    if (offset < bank_size)
      assert(!pet_cmedia_speech_stage(bank_size + 1, offset, bank + offset, 1, NULL, 0));
  }
}

static void touch(const char *name) {
  FILE *file = fopen(name, "wb");
  assert(file && fclose(file) == 0);
}

static void test_remove_slot(void) {
  const char *keep[] = {"p4m08-0-s0.bin", "p4m07-1-b4.tmp", "p4m07-1-a1.bin",
                        "p4m07-1-c1.bin.old", "as-private-0"};
  for (unsigned i=0; i<sizeof(keep)/sizeof(keep[0]); ++i) touch(keep[i]);
  scan_calls=unlink_calls=0;
  pet_cmedia_remove(7);
  assert(scan_calls==1 && unlink_calls==0);

  /* Populate the full historical sweep, including unused clip/image indices. */
  for (unsigned gen=0; gen<2; ++gen) {
    for (const char *kind="scba"; *kind; ++kind) {
      unsigned count = *kind=='a' ? 1 : 4;
      for (unsigned index=0; index<count; ++index) {
        for (unsigned tmp=0; tmp<2; ++tmp) {
          char name[32];
          snprintf(name,sizeof(name),"p4m07-%u-%c%u.%s",gen,*kind,index,tmp?"tmp":"bin");
          touch(name);
        }
      }
    }
  }
  scan_calls=unlink_calls=0;
  g_pending.serial=1;
  pet_cmedia_remove(7);
  assert(scan_calls==0 && unlink_calls==0);
  g_pending.serial=0;

  /* Interrupt physical reclamation. The following call rebuilds its bitmap
   * from disk, including a file recreated in the already-cleared subset. */
  fail_after_unlinks=11;
  pet_cmedia_remove(7);
  assert(scan_calls==1 && unlink_calls==52 && access("p4m07-0-s0.bin", F_OK)!=0);
  assert(access("p4m07-1-a0.tmp", F_OK)==0);
  touch("p4m07-0-s0.bin");
  fail_after_unlinks=0;
  scan_calls=unlink_calls=0;
  pet_cmedia_remove(7);
  assert(scan_calls==1 && unlink_calls==42);
  scan_calls=unlink_calls=0;
  pet_cmedia_remove(7);
  assert(scan_calls==1 && unlink_calls==0);

  touch("p4m07-1-c3.tmp");
  scan_calls=unlink_calls=0; fail_scans=1;
  pet_cmedia_remove(7);
  assert(scan_calls==2 && unlink_calls==1 && access("p4m07-1-c3.tmp",F_OK)!=0);
  touch("p4m07-0-b3.bin");
  scan_calls=unlink_calls=0; fail_scans=2;
  pet_cmedia_remove(7);
  assert(scan_calls==2 && unlink_calls==52 && access("p4m07-0-b3.bin",F_OK)!=0);
  fail_scans=0;
  for (unsigned i=0; i<sizeof(keep)/sizeof(keep[0]); ++i) {
    assert(access(keep[i],F_OK)==0 && unlink(keep[i])==0);
  }
}

int main(void) {
  test_remove_slot();
  uint8_t *a = make_bank(17), *b = make_bank(42);
  pet_cext_runtime_t original = {0}, replacement = {0}, empty = {0};
  set_hash(&original, a);
  set_hash(&replacement, b);
  clock_t start = clock();
  stage(a);
  clock_t staged = clock();
  assert(!pet_cmedia_stage_matches(&empty, NULL, 0));
  assert(!pet_cmedia_stage_matches(&replacement, NULL, 0));
  assert(pet_cmedia_stage_matches(&original, NULL, 0));
  g_speech_stage.bytes[8 + TEST_CUES * 8 + 3] = 1;
  assert(!pet_cext_speech_bank_valid(g_speech_stage.bytes, bank_size));
  g_speech_stage.bytes[8 + TEST_CUES * 8 + 3] = 0;
  assert(pet_cmedia_persist(0, 0, &original));
  clock_t committed = clock();
  assert(pet_cmedia_speech_verify_file(0, 0, original.speech_sha256));
  assert(!pet_cmedia_speech_verify_file(0, 0, replacement.speech_sha256));
  pet_cmedia_stage_reset();

  clock_t loading = clock();
  assert(pet_cmedia_load(0, 0, &original));
  clock_t loaded = clock();
  uint32_t old_generation = pet_cmedia_generation();
  pet_cmedia_speech_lease_t previous = {0}, current = {0};
  assert(pet_cmedia_speech_pin_try_at(TEST_CUES - 1, old_generation, &previous));
  assert(previous.data[0] == 17);
  printf("HOST bytes=%zu stage_cpu_ms=%.3f commit_cpu_ms=%.3f load_cpu_ms=%.3f first_cue_cpu_ms=%.3f\n", bank_size, 1000.0*(staged-start)/CLOCKS_PER_SEC, 1000.0*(committed-staged)/CLOCKS_PER_SEC, 1000.0*(loaded-loading)/CLOCKS_PER_SEC, 1000.0*(clock()-loaded)/CLOCKS_PER_SEC);
  assert(!pet_cmedia_speech_pin_try_at(TEST_CUES, old_generation, &current));

  stage(b);
  assert(pet_cmedia_persist(0, 1, &replacement));
  pet_cmedia_ticket_t ticket = {0};
  assert(pet_cmedia_prepare(0, 1, &replacement, &ticket));
  /* Preparation cannot make the new generation visible. */
  assert(!pet_cmedia_speech_pin_try_at(0, old_generation + 1, &current));
  assert(pet_cmedia_commit(&ticket));
  uint32_t new_generation = pet_cmedia_generation();
  assert(new_generation != old_generation);
  assert(!pet_cmedia_speech_pin_try_at(0, old_generation, &current));
  assert(pet_cmedia_speech_pin_try_at(0, new_generation, &current));
  assert(current.data[0] == 42 && previous.data[0] == 17);
  assert(current.bytes == TEST_BLOCKS * PET_CEXT_SPEECH_BLOCK_BYTES);
  assert(pet_cmedia_unpin_deferred(previous.token));
  pet_cmedia_collect();
  assert(current.data[0] == 42);

  /* Existing media plus the two committed banks leaves no room for staging. */
  FILE *fill = local_open("/spiffs/p4m01-0-b0.bin", "wb");
  assert(fill && ftruncate(fileno(fill), PET_CMEDIA_DEVICE_BUDGET_BYTES - 2 * bank_size) == 0);
  assert(fclose(fill) == 0);
  assert(!pet_cmedia_persist(1, 0, &replacement));
  assert(!pet_cmedia_persist(0, 0, &replacement));
  assert(local_unlink("/spiffs/p4m01-0-b0.bin") == 0);
  assert(pet_cmedia_persist(0, 0, &replacement));
  assert(pet_cmedia_speech_verify_file(0, 0, replacement.speech_sha256));
  assert(!pet_cmedia_speech_verify_file(0, 0, original.speech_sha256));

  char path[32];
  speech_path(0, 0, false, path);
  FILE *corrupt = local_open(path, "r+b");
  assert(corrupt && fseek(corrupt, (long)bank_size - 1, SEEK_SET) == 0);
  assert(fputc(16, corrupt) == 16 && fclose(corrupt) == 0);
  assert(!pet_cmedia_speech_verify_file(0, 0, replacement.speech_sha256));
  assert(!pet_cmedia_prepare(0, 0, &replacement, &ticket));
  assert(pet_cmedia_generation() == new_generation);
  assert(current.data[0] == 42);
  assert(pet_cmedia_unpin(current.token));
  pet_cmedia_unload();
  assert(!pet_cmedia_speech_pin_try_at(0, new_generation, &current));

  pet_cmedia_stage_reset();
  pet_cmedia_remove(0);
  assert(!pet_cmedia_speech_verify_file(0, 1, replacement.speech_sha256));
  free(a);
  free(b);
  return 0;
}
