"""Exercise the actual generated ESP-IDF v3 interrupt paths with a fake DMA latch."""
from pathlib import Path
import subprocess
import sys

from p4_boot_dma_test import v3_idf

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIPI_DSI_BRG_LL_EVENT_VSYNC (1 << 1)
#define MIPI_DSI_BRG_LL_EVENT_UNDERRUN (1 << 0)
#define ESP_DRAM_LOGE(...) ((void)0)
#define __containerof(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
typedef struct { int unused; } esp_lcd_panel_t;
typedef esp_lcd_panel_t *esp_lcd_panel_handle_t;
typedef int dw_gdma_channel_handle_t, dw_gdma_trans_done_event_data_t, dw_gdma_link_list_handle_t;
typedef struct { bool is_valid, is_last; } dw_gdma_block_markers_t;
typedef struct { void *bridge; } mipi_dsi_hal_context_t;
typedef struct { mipi_dsi_hal_context_t hal; } test_bus_t;

typedef struct {
    esp_lcd_panel_t base;
    test_bus_t *bus;
    unsigned cur_fb_index;
    int link_lists[2];
    bool pet_p4_refresh_after_dma;
    bool (*on_refresh_done)(esp_lcd_panel_handle_t, void *, void *);
    void *user_ctx;
} esp_lcd_dpi_panel_t;

static unsigned notifications;
static unsigned intr_status;
static int linked, restarted;
static bool assert_dma;
static void dw_gdma_lli_set_block_markers(int node, dw_gdma_block_markers_t markers) {
    assert(node && markers.is_valid && markers.is_last);
}
static int dw_gdma_link_list_get_item(int list, int index) { assert(index == 0); return list; }
static void dw_gdma_channel_use_link_list(int chan, int list) { assert(chan == 1); linked = list; }
static void dw_gdma_channel_enable_ctrl(int chan, bool enabled) {
    assert(chan == 1 && enabled);
    restarted = linked;
}
static uint32_t mipi_dsi_brg_ll_get_interrupt_status(void *bridge) {
    (void)bridge;
    return intr_status;
}
static void mipi_dsi_brg_ll_clear_interrupt_status(void *bridge, uint32_t status) {
    (void)bridge;
    assert(status == intr_status);
}
static void portYIELD_FROM_ISR(void) {}
static bool refresh(esp_lcd_panel_handle_t panel, void *event, void *user_ctx) {
    assert(panel && !event && !user_ctx);
    if (assert_dma) assert(restarted == 22);
    notifications++;
    return true;
}
'''

SCENARIOS = r'''
int main(void) {
    test_bus_t bus = {0};
    esp_lcd_dpi_panel_t panel = {
        .bus = &bus, .link_lists = {11,22}, .on_refresh_done = refresh,
    };
    intr_status = MIPI_DSI_BRG_LL_EVENT_VSYNC;
    // Legacy panel still gets a single callback, from bridge VSYNC.
    assert(!mipi_dsi_dma_trans_done_cb(1, NULL, &panel));
    assert(restarted == 11 && notifications == 0);
    mipi_dsi_bridge_isr_handler(&panel);
    assert(notifications == 1);

    // NV3049 waits for the new DMA link to start even if bridge VSYNC fires first.
    pet_p4_lcd_use_dma_refresh(&panel.base);
    panel.cur_fb_index = 1;
    assert_dma = true;
    mipi_dsi_bridge_isr_handler(&panel);
    assert(notifications == 1 && restarted == 11);
    assert(mipi_dsi_dma_trans_done_cb(1, NULL, &panel));
    assert(notifications == 2 && restarted == 22);
    mipi_dsi_bridge_isr_handler(&panel);
    assert(notifications == 2);
}
'''


def test_v3_handoff_is_per_panel_and_follows_dma_latch(tmp_path):
    original = v3_idf() / "components/esp_lcd/dsi/esp_lcd_panel_dpi.c"
    generated = tmp_path / "generated.c"
    subprocess.run(
        [sys.executable, str(ROOT / "tools/patch_p4_lcd_underrun.py"), str(original),
         str(generated), "--skip-underrun", "--dma-refresh"],
        check=True,
    )
    driver = generated.read_text()
    assert "cbs->on_refresh_done != NULL && !dpi_panel->pet_p4_refresh_after_dma" in driver
    assert "pet_p4_lcd_on_underrun" not in driver  # Pico has no Wi-Fi underrun hook.
    selected = []
    for start in ("bool mipi_dsi_dma_trans_done_cb(", "void pet_p4_lcd_use_dma_refresh(",
                  "void mipi_dsi_bridge_isr_handler("):
        selected.append(start + driver.split(start, 1)[1].split("\n}\n", 1)[0] + "\n}\n")
    source = tmp_path / "handoff.c"
    source.write_text(HARNESS + "\n".join(selected) + SCENARIOS)
    executable = tmp_path / "handoff"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         str(source), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)


def test_draw_recovery_never_writes_a_possible_dma_front(tmp_path):
    from p4_render_trace_test import function
    from p4_h264_prefetch_test import compile_run

    lcd = (ROOT / 'main/pet_p4_lcd.c').read_text()
    state = lcd[lcd.index('static enum { NV_READY'):lcd.index('static ppa_client_handle_t')]
    functions = '\n'.join(function(lcd, name) for name in (
        'lcd_refresh_done_callback', 'nv_refresh_done_callback', 'nv_wait_for_refresh',
        'nv_publish', 'nv_recover', 'lcd_draw_core'))
    compile_run(tmp_path, r'''
#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pet_p4_lcd_profile.h"
#define BSP_LCD_PANEL_AUTO 1
#define BSP_LCD_H_RES 2
#define BSP_LCD_V_RES 2
#define ESP_LOGW(...) ((void)0)
#define IRAM_ATTR
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE };
enum { LCD_REFRESH_WAIT_TICKS = 250, pdTRUE = 1, pdFALSE = 0, PET_LCD_PANEL_NV3049 = 2 };
typedef int esp_err_t, BaseType_t;
typedef unsigned TickType_t;
typedef void *esp_lcd_panel_handle_t, *SemaphoreHandle_t;
typedef void esp_lcd_dpi_panel_event_data_t;
static uint8_t fb[2][12], *g_nv_buffers[2] = {fb[0], fb[1]};
static void *g_panel_handle = fb, *g_refresh_done_sem = &fb[1];
static unsigned g_nv_front, g_nv_last_refresh_tick, tick, refresh_at, writes, submits, reveals;
static unsigned conversions;
static unsigned selected, scanning, caller = 1, core, g_nv_render_task = 1;
static atomic_uint g_nv_refresh, g_nv_max_refresh_gap;
static atomic_bool g_nv_ppa_active;
static bool g_backlight_revealed, token, drop_give, fail_submit, publish_on_failure, irq_in_submit;
static int xPortGetCoreID(void) { return core; }
static unsigned xTaskGetCurrentTaskHandle(void) { return caller; }
static unsigned xTaskGetTickCount(void) { return tick; }
static unsigned xTaskGetTickCountFromISR(void) { return tick; }
static void xSemaphoreGiveFromISR(void *sem, int *awake) {
  assert(sem == g_refresh_done_sem);
  *awake = token = !drop_give;
}
static bool nv_refresh_done_callback(void *, void *, void *);
static void latch(void) {
  scanning = selected;
  nv_refresh_done_callback(g_panel_handle, NULL, g_refresh_done_sem);
}
static int xSemaphoreTake(void *sem, unsigned ticks) {
  assert(sem == g_refresh_done_sem);
  if (!token) {
    tick += ticks;
    if (refresh_at && tick >= refresh_at) { refresh_at = 0; latch(); }
  }
  bool result = token;
  token = false;
  return result;
}
static void nv_init_conversion(void) { assert(scanning == 0); conversions++; }
static int pet_p4_lcd_panel_model(void) { return PET_LCD_PANEL_NV3049; }
static int nv_convert_ppa(const uint16_t *p, unsigned w, unsigned h, uint8_t *out) {
  (void)p; (void)w; (void)h; (void)out; assert(0); return 1;
}
static bool pet_nv3049_blit_rgb565(uint8_t *out, int x, int y, int w, int h, const uint16_t *p) {
  assert(out != fb[scanning]);
  writes++;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      memset(out + ((y+j)*2+x+i)*3, *p++, 3);
  return true;
}
static int esp_lcd_panel_draw_bitmap(void *panel, int x, int y, int w, int h, const void *buf) {
  assert(panel == g_panel_handle && !x && !y && w == 2 && h == 2);
  assert(buf == fb[0] || buf == fb[1]);
  submits++;
  if (!fail_submit || publish_on_failure) selected = buf == fb[1];
  if (irq_in_submit) latch();
  return fail_submit ? 57 : ESP_OK;
}
static int pet_p4_lcd_reveal(void) { reveals++; g_backlight_revealed = true; return ESP_OK; }
''' + state + functions + r'''
static uint16_t pixels[4] = {1,2,3,4};
static int draw(void) { return lcd_draw_core(0,0,2,2,pixels,NULL); }
int main(void) {
  // Even a real latch inside submit predates the recorded fence. Its token
  // cannot authorize reuse. Repeated draws during the outage do no work.
  atomic_store(&g_nv_refresh, UINT_MAX-1);
  irq_in_submit = true;
  assert(draw() == ESP_ERR_TIMEOUT && scanning == 1 && writes == 1 && submits == 1);
  uint8_t frozen[2][12];
  memcpy(frozen, fb, sizeof(fb));
  unsigned stopped_tick = tick;
  for (int i = 0; i < 1000; i++) assert(draw() == ESP_ERR_TIMEOUT);
  assert(tick == stopped_tick && g_nv_front == 0);
  assert(!memcmp(frozen, fb, sizeof(fb)) && writes == 1 && submits == 1 && reveals == 0);
  irq_in_submit = false;
  latch(); // late, counter wraps, semaphore can also lose its give
  caller++; assert(draw() == ESP_ERR_INVALID_STATE); caller--;
  core++; assert(draw() == ESP_ERR_INVALID_STATE); core--;
  assert(writes == 1 && submits == 1 && !g_nv_recoveries);
  drop_give = true; refresh_at = tick + 250;
  assert(draw() == ESP_OK && scanning == 0 && g_nv_front == 0);
  assert(writes == 2 && submits == 2 && reveals == 1 && g_nv_recoveries == 1);
  assert(g_nv_max_recovery_gap == 500 && g_nv_last_draw_tick == tick);

  for (int changed = 0; changed <= 1; changed++) {
    unsigned front = scanning;
    fail_submit = true; publish_on_failure = changed;
    pixels[0]++;
    assert(draw() == 57);
    latch(); // may have latched either buffer after the failed submission
    assert(scanning == (front ^ changed));
    memcpy(frozen, fb, sizeof(fb));
    unsigned before = writes;
    assert(draw() == 57); // even the recovery publication failed
    assert(!memcmp(frozen, fb, sizeof(fb)) && writes == before);
    fail_submit = false;
    assert(draw() == ESP_ERR_TIMEOUT); // confirmed image republished, no fresh IRQ
    assert(selected == front && !memcmp(frozen, fb, sizeof(fb)) && writes == before);
    assert(draw() == ESP_ERR_TIMEOUT && writes == before);
    latch();
    refresh_at = tick + 250;
    uint16_t patch = 99;
    assert(lcd_draw_core(1,1,1,1,&patch,NULL) == ESP_OK);
    assert(writes == before+1 && !memcmp(fb[scanning], frozen[front], 9));
    assert(fb[scanning][9] == 99 && scanning == (front ^ 1));
  }
  assert(reveals == 1 && g_nv_recoveries == 3);
  // Boot's initial-fence timeout defers even the PPA self-test, which writes fb1.
  selected = scanning = g_nv_published_front = 0;
  g_nv_published_at = atomic_load(&g_nv_refresh);
  g_nv_state = NV_FIRST_LATCH;
  assert(draw() == ESP_ERR_TIMEOUT && !conversions);
  latch(); refresh_at = tick + 250;
  assert(draw() == ESP_OK && conversions == 1 && g_nv_front == 1);
  return 0;
}
''', 'nv-recovery')
