/* Runs against the real interpreter and browser preview without live networking. */
#include "hachi.h"
static char output[512];
static const char request[] = "{\"op\":\"credential.get\",\"key\":\"session\"}";
static void check(int ok) {
  if (!ok)
    __builtin_trap();
}
HACHI_EXPORT(hachi_init) void hachi_init(void) {
  check(hachi_service_submit(request, sizeof(request) - 1) == -1);
  check(hachi_service_poll(output, sizeof output) == -1);
  check(hachi_service_read(1, 0, output, sizeof output) == -1);
  check(hachi_service_release(1) == -1);
  check(hachi_service_submit((const char *)131071, 8192) == -3);
  check(hachi_service_poll(output, 0) == -3);
  for (int i = 0; i < 4; i++)
    check(hachi_service_release(1) == -1);
  check(hachi_service_release(1) == -3);
}
HACHI_EXPORT(hachi_event) void hachi_event(int type, int a, int b) {
  (void)type;
  (void)a;
  (void)b;
  check(hachi_service_poll(output, sizeof output) == -1);
}
HACHI_EXPORT(hachi_frame) void hachi_frame(int elapsed) {
  (void)elapsed;
  check(hachi_service_poll(output, sizeof output) == -1);
}
